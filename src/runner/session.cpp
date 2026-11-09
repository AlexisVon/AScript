#include "session.h"

#include "core/core.h"
#include "core/extend.h"
#include "core/inspect.h"
#include "core/project.h"

#include "watchdog.h"

#include <filesystem>
#include <string>

// session — engine creation and one run; everything leaves through writer
namespace runner {

    session::~session() {
        m_hook.shutdown();
        if (m_engine) core::destroy(m_engine);
        core::inspect::set_child_observer(nullptr, nullptr);
    }

    void session::run(const proto::exec_request& _req) {
        m_console = _req.console;
        if (!_req.cwd.empty()) {
            std::error_code ec;
            std::filesystem::current_path(_req.cwd, ec);
            if (ec) m_out.send_event(proto::make_log("warn", "cannot chdir to " + _req.cwd + ": " + ec.message()));
        }

        alx::varmap cfg;
        std::string main_path;
        std::vector<std::string> paths;
        std::string err;
        if (!resolve(_req, cfg, main_path, paths, err)) {
            fail(err);
            return;
        }

        // the root identity is what breakpoints in the executed script bind to
        const std::string root_path = _req.mode == "text" ? _req.home : main_path;
        std::string root_id = _req.source_id;
        if (root_id.empty()) {
            if (root_path.empty()) {
                root_id = "text";
            } else {
                std::error_code ec;
                const std::filesystem::path canon = std::filesystem::weakly_canonical(root_path, ec);
                root_id = "file:" + (ec ? root_path : canon.string());
            }
        }
        m_src.set_root(root_id, root_path);

        const bool debug = _req.debug || _req.stop_on_entry || !_req.breakpoints.empty();
        const bool mt = debug && _req.mt_debug;

        // console: keep the engine, but rebuild it before arming this run's hook
        const bool reset = m_ctl.consume_reset();
        if (m_engine && (reset || m_engine->config().debug_enable != debug)) {
            m_hook.shutdown();
            core::destroy(m_engine);
            m_engine = nullptr;
            core::inspect::set_child_observer(nullptr, nullptr);
        }
        m_hook.configure(_req.breakpoints, _req.stop_on_entry, _req.skip_traps, mt);
        if (mt) core::inspect::set_child_observer(&hook::child_observer, &m_hook);
        if (!m_engine) {
            m_engine = core::create(core::varmap_to_config(cfg));
            m_engine->on_cerr.connect([this](const std::string& _s) { m_out.output(_s); });
            m_engine->on_cmpl.connect([this](const alx::script::compile_error& _e) {
                m_out.send_event(proto::make_compile_error(_e.loc.path, _e.loc.row, _e.loc.col, _e.msg));
            });
            m_engine->set_pipe(&session::pipe_in_cb, &session::pipe_out_cb, this, this);
        }
        m_engine->set_search_paths(std::list<std::string>(paths.begin(), paths.end()));

        const alx::uint_64 interval = _req.hook_interval ? _req.hook_interval : (debug ? 1024 : 0);
        m_engine->set_hook(interval ? &hook::entry : nullptr, interval ? m_hook.main_ud() : nullptr,
                           interval);

        m_ctl.attach_engine(m_engine);
        // stop may have landed while the engine was being created; set_interrupt would be discarded
        if (m_ctl.stop_requested()) {
            alx::script::engine::result stopped;
            stopped.error = alx::script::error_type::InterruptedError;
            stopped.value = alx::variant(std::string("stopped by host"));
            m_ctl.detach_engine();
            finish(false, stopped);
            return;
        }

        watchdog wd(m_engine, _req.wall_ms);
        wd.start();

        alx::script::engine::result res;
        if (_req.mode == "text") {
            alx::bytes src(_req.text);
            res = m_engine->exec(alx::bytes_view(src), _req.home);
        } else {
            res = m_engine->exec(main_path);
        }

        wd.stop();
        m_ctl.detach_engine();
        if (wd.fired()) m_out.send_event(proto::make_log("warn", "wall-clock limit reached; run interrupted"));
        finish(res.error == alx::script::error_type::NoError, res);
    }

    bool session::resolve(const proto::exec_request& _req, alx::varmap& _cfg, std::string& _main,
                          std::vector<std::string>& _paths, std::string& _err) {
        std::string script_dir;
        if (_req.mode == "ax") {
            core::project::ax_config ax;
            if (!core::project::parse_ax(_req.path, ax, _err)) {
                _err = "cannot read project " + _req.path + ": " + _err;
                return false;
            }
            _cfg = ax.engine;
            _main = ax.main;
            _paths.assign(ax.path.begin(), ax.path.end());
            script_dir = ax.dir;
        } else if (_req.mode == "text") {
            script_dir = _req.home.empty() ? _req.cwd : _req.home;
        } else {
            _main = _req.path;
            const size_t slash = _main.find_last_of("/\\");
            script_dir = slash == std::string::npos ? "." : _main.substr(0, slash);
            if (_main.size() > 4 && _main.compare(_main.size() - 4, 4, ".axp") == 0) {
                std::string hint_err;
                for (const auto& p : core::project::axp_search_paths(_main, hint_err))
                    _paths.push_back(core::project::expand_path(p, script_dir));
            }
        }
        // user library dirs (raw expressions) then the host's system dirs
        for (const auto& p : _req.search_paths)
            _paths.push_back(core::project::expand_path(p, script_dir));
        for (const auto& p : core::project::system_library_paths()) _paths.push_back(p);

        // breakpoints and stopOnEntry imply position tracking, even without an explicit debug flag
        const bool debug = _req.debug || _req.stop_on_entry || !_req.breakpoints.empty();
        _cfg["debug_enable"] = alx::variant(debug);
        if (_req.overflow_check) _cfg["overflow_check"] = alx::variant(true);
        if (_req.max_stack) _cfg["max_stack"] = alx::variant(alx::uint_64(_req.max_stack));
        if (_req.max_vecfill) _cfg["max_vecfill"] = alx::variant(alx::uint_64(_req.max_vecfill));
        if (_req.parse_depth) _cfg["parse_depth"] = alx::variant(alx::uint_64(_req.parse_depth));
        return true;
    }

    void session::finish(bool _ok, const alx::script::engine::result& _res) {
        m_ctl.mark_idle();
        std::string value;
        std::string message;
        if (_ok) {
            if (!_res.value.null()) value = core::utils::repr(_res.value);
        } else if (_res.value.is<std::string>()) {
            message = _res.value.to<std::string>();
        }

        // destroy first: a link's unload output is queued ahead of the result, as it ran
        m_hook.finish(m_console);
        if (m_engine && !m_console) {
            core::destroy(m_engine);
            m_engine = nullptr;
            core::inspect::set_child_observer(nullptr, nullptr);
        }
        m_out.sync_log(); // the log is complete before the result says the run is over

        const int code = _ok ? 0 : static_cast<int>(_res.error);
        m_out.send_event(proto::make_result(_ok, value, _res.elapsed_us, static_cast<int>(_res.error),
                                            alx::script::error_type_name(_res.error), message));
        if (!m_console) m_out.send_event(proto::make_exited(code, false, ""));
        m_ctl.set_exit_code(code);
    }

    void session::fail(const std::string& _msg) {
        m_ctl.mark_idle();
        m_hook.finish(m_console);
        if (m_engine && !m_console) {
            core::destroy(m_engine);
            m_engine = nullptr;
            core::inspect::set_child_observer(nullptr, nullptr);
        }
        m_out.sync_log();
        m_out.send_event(proto::make_result(false, "", 0, static_cast<int>(alx::script::error_type::RuntimeError),
                                            "RuntimeError", _msg));
        if (!m_console) m_out.send_event(proto::make_exited(2, false, ""));
        m_ctl.set_exit_code(2);
    }

    void session::pipe_out_cb(const std::string* _s, void* _ud) {
        if (!_s || !_ud) return;
        static_cast<session*>(_ud)->m_out.output(*_s);
    }

    std::string session::pipe_in_cb(void* _ud) {
        auto* self = static_cast<session*>(_ud);
        if (!self) return std::string();
        self->m_out.send_event(proto::make_input_request(""));
        return self->m_ctl.wait_input();
    }

} // namespace runner
