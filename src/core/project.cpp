#include "project.h"

#include "core.h"

#include "afile.h"
#include "aplatform.h"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <vector>

namespace core {
    namespace project {

        namespace {

            // join — resolve _rel against _dir (absolute inputs pass through)
            static std::string join(const std::string& _dir, const std::string& _rel) {
                if (_rel.empty() || alx::file_info::is_absolute(_rel)) return _rel;
                std::string p = _dir + "/" + _rel;
                if (p.rfind("./", 0) == 0) p = p.substr(2);
                return p;
            }

            // ensure_parent — create the output directory chain before writing
            static bool ensure_parent(const std::string& _file, std::string& _err) {
                const std::filesystem::path parent = std::filesystem::path(_file).parent_path();
                if (parent.empty()) return true;
                std::error_code ec;
                std::filesystem::create_directories(parent, ec);
                if (ec) {
                    _err = "cannot create " + parent.string() + ": " + ec.message();
                    return false;
                }
                return true;
            }

        } // namespace

        namespace {
            std::string exe_dir() {
                const std::string p = alx::process_info::current().path;
                const size_t slash = p.find_last_of("/\\");
                return slash == std::string::npos ? "." : p.substr(0, slash);
            }

            // build_hint — write the merged hint JSON (existing keys + search_paths) to a temp file
            bool build_hint(const std::string& _base, const std::vector<std::string>& _raw,
                            std::string& _path, std::string& _temp, std::string& _err) {
                _path = _base;
                _temp.clear();
                if (_raw.empty()) return true;
                alx::json_object merged;
                if (!_base.empty()) {
                    const alx::bytes data = alx::file::read_all(_base);
                    if (data.empty()) {
                        _err = "cannot read hint " + _base;
                        return false;
                    }
                    bool ok = false;
                    const alx::json_value v = alx::json_doc::from_value(alx::bytes_view(data), &ok);
                    if (!ok || !v.is<alx::json_object>()) {
                        _err = "hint must be a JSON object: " + _base;
                        return false;
                    }
                    const alx::json_object& o = v.to<alx::json_object>();
                    for (auto it = o.begin(); it != o.end(); ++it)
                        if (it.key() != "search_paths") merged.insert(it.key(), it.value());
                }
                alx::json_array arr;
                for (const auto& p : _raw) arr.append(alx::json_value(p));
                merged.insert("search_paths", alx::json_value(arr));
                const std::string text = alx::json_doc::to_json(std::move(merged), true);
                static int seq = 0;
                const std::string name = "ascript-hint-" +
                                         std::to_string(alx::process_info::current().pid) + "-" +
                                         std::to_string(++seq) + ".json";
                _temp = (std::filesystem::temp_directory_path() / name).string();
                if (!alx::file::write_all(_temp, text.data(), text.size(), false)) {
                    _err = "cannot write hint " + _temp;
                    _temp.clear();
                    return false;
                }
                _path = _temp;
                return true;
            }

            void drop_hint(const std::string& _temp) {
                if (!_temp.empty()) std::remove(_temp.c_str());
            }

            // the three host dirs travel as expressions so an axp stays relocatable
            void append_system_exprs(std::vector<std::string>& _raw) {
                _raw.push_back("{exe}");
                _raw.push_back("{exe}/links");
                _raw.push_back("{exe}/imports");
            }
        } // namespace

        std::string expand_path(const std::string& _expr, const std::string& _script_dir) {
            static const std::string kScript = "{script}";
            static const std::string kExe = "{exe}";
            if (_expr.rfind(kScript, 0) == 0) return _script_dir + _expr.substr(kScript.size());
            if (_expr.rfind(kExe, 0) == 0) return exe_dir() + _expr.substr(kExe.size());
            return join(_script_dir, _expr);
        }

        std::vector<std::string> system_library_paths() {
            return {(exe_dir()), (exe_dir() + "/links"), (exe_dir() + "/imports")};
        }

        bool parse_ax(const std::string& _path, ax_config& _out, std::string& _err) {
            const alx::bytes data = alx::file::read_all(_path);
            if (data.empty()) {
                _err = "cannot read " + _path;
                return false;
            }
            bool ok = false;
            alx::json_value v = alx::json_doc::from_value(alx::bytes_view(data), &ok);
            if (!ok || !v.is<alx::json_object>()) {
                _err = "invalid JSON in " + _path;
                return false;
            }
            const alx::json_object& o = v.to<alx::json_object>();

            if (!o.contain("main") || !o.value("main").is<std::string>() ||
                o.value("main").to<std::string>().empty()) {
                _err = "missing \"main\" in " + _path;
                return false;
            }

            _out = ax_config();
            _out.file = _path;
            const size_t slash = _path.find_last_of("/\\");
            _out.dir = slash == std::string::npos ? "." : _path.substr(0, slash);
            if (o.contain("name") && o.value("name").is<std::string>())
                _out.name = o.value("name").to<std::string>();
            if (o.contain("out") && o.value("out").is<std::string>()) {
                const std::string d = o.value("out").to<std::string>();
                if (!d.empty()) _out.out = join(_out.dir, d);
            }
            _out.main = join(_out.dir, o.value("main").to<std::string>());

            if (o.contain("engine") && o.value("engine").is<alx::json_object>())
                _out.engine = o.value("engine").to<alx::json_object>().to_varmap();

            if (o.contain("path") && o.value("path").is<alx::json_array>()) {
                for (const alx::json_value* e : o.value("path").to<alx::json_array>())
                    if (e->is<std::string>()) {
                        const std::string raw = e->to<std::string>();
                        _out.path_expr.push_back(raw);
                        _out.path.push_back(expand_path(raw, _out.dir));
                    }
            }

            if (o.contain("docpath") && o.value("docpath").is<alx::json_array>()) {
                for (const alx::json_value* e : o.value("docpath").to<alx::json_array>())
                    if (e->is<std::string>())
                        _out.docpath.push_back(expand_path(e->to<std::string>(), _out.dir));
            }

            if (o.contain("hint") && o.value("hint").is<std::string>()) {
                const std::string h = o.value("hint").to<std::string>();
                if (!h.empty()) _out.hint = join(_out.dir, h);
            }

            // hook config (debug support)
            if (o.contain("hook") && o.value("hook").is<bool>())
                _out.hook = o.value("hook").to<bool>();
            if (o.contain("hook_freq") && o.value("hook_freq").is<alx::int_64>())
                _out.hook_freq = static_cast<alx::uint_64>(o.value("hook_freq").to<alx::int_64>());

            return true;
        }

        int run(const ax_config& _ax, alx::script::hook_fn _hook) {
            auto* eng = core::create(core::varmap_to_config(_ax.engine));
            eng->on_cerr.connect([](const std::string& _s) {
                std::cerr << _s;
                if (_s.empty() || _s.back() != '\n') std::cerr << "\n";
            });
            eng->on_cmpl.connect([](const alx::script::compile_error& _e) {
                std::cerr << _e.loc.row << ":" << _e.loc.col << ": " << _e.msg << "\n";
            });
            // hook: use _hook only if ax.hook is true
            if (_hook && _ax.hook)
                eng->set_hook(_hook, nullptr, _ax.hook_freq);
            std::vector<std::string> paths = _ax.path;
            for (const auto& p : system_library_paths()) paths.push_back(p);
            eng->set_search_paths(std::list<std::string>(paths.begin(), paths.end()));
            const alx::script::engine::result res = eng->exec(_ax.main);
            core::destroy(eng);
            return static_cast<int>(res.error);
        }

        // default_out — <ax basename>.axp in the configured out dir, else next to the .ax
        static std::string default_out(const ax_config& _ax) {
            const size_t slash = _ax.file.find_last_of("/\\");
            std::string base = slash == std::string::npos ? _ax.file : _ax.file.substr(slash + 1);
            const size_t dot = base.find_last_of('.');
            if (dot != std::string::npos) base = base.substr(0, dot);
            const std::string dir = _ax.out.empty() ? _ax.dir : _ax.out;
            return dir + "/" + base + ".axp";
        }

        bool compile(const ax_config& _ax, bool _embed, const std::string& _out,
                     std::string& _err, bool _cmps, int _debug,
                     const std::vector<std::string>& _extra_paths) {
            alx::varmap engine = _ax.engine;
            if (_debug >= 0) engine["debug_enable"] = alx::variant(_debug == 1);
            auto* eng = core::create(core::varmap_to_config(engine));
            std::string cmpl_err;
            eng->on_cerr.connect([&](const std::string& _s) { cmpl_err += _s; });
            eng->on_cmpl.connect([&](const alx::script::compile_error& _e) {
                cmpl_err += std::to_string(_e.loc.row) + ":" + std::to_string(_e.loc.col) + ": " +
                            _e.msg + "\n";
            });
            std::vector<std::string> paths = _ax.path;
            for (const auto& p : _extra_paths) paths.push_back(expand_path(p, _ax.dir));
            for (const auto& p : system_library_paths()) paths.push_back(p);
            eng->set_search_paths(std::list<std::string>(paths.begin(), paths.end()));
            std::vector<std::string> raw = _ax.path_expr;
            for (const auto& p : _extra_paths) raw.push_back(p);
            append_system_exprs(raw);
            std::string hint, temp;
            if (!build_hint(_ax.hint, raw, hint, temp, _err)) {
                core::destroy(eng);
                return false;
            }
            // hint passed as JSON file path — the engine reads + compresses it (cmps-following)
            const alx::bytes bin = eng->compile(_ax.main, _cmps, _embed, hint);
            drop_hint(temp);
            core::destroy(eng);
            if (bin.empty()) {
                _err = cmpl_err.empty() ? "compile failed" : cmpl_err;
                return false;
            }
            const std::string target = _out.empty() ? default_out(_ax) : _out;
            if (!ensure_parent(target, _err)) return false;
            if (!alx::file::write_all(target, bin.data(), bin.size(), false)) {
                _err = "cannot write " + target;
                return false;
            }
            return true;
        }

        bool compile_file(const std::string& _file, const std::string& _out, bool _debug,
                          bool _embed, const ax_config* _project, std::string& _err, bool _cmps,
                          const std::vector<std::string>& _extra_paths) {
            alx::varmap engine;
            if (_project) engine = _project->engine;
            engine["debug_enable"] = alx::variant(_debug);
            auto* eng = core::create(core::varmap_to_config(engine));
            std::string cmpl_err;
            eng->on_cerr.connect([&](const std::string& _s) { cmpl_err += _s; });
            eng->on_cmpl.connect([&](const alx::script::compile_error& _e) {
                cmpl_err += std::to_string(_e.loc.row) + ":" + std::to_string(_e.loc.col) + ": " +
                            _e.msg + "\n";
            });
            const std::string script_dir = _project ? _project->dir : ".";
            std::vector<std::string> paths = _project ? _project->path : std::vector<std::string>();
            for (const auto& p : _extra_paths) paths.push_back(expand_path(p, script_dir));
            for (const auto& p : system_library_paths()) paths.push_back(p);
            eng->set_search_paths(std::list<std::string>(paths.begin(), paths.end()));
            std::vector<std::string> raw = _project ? _project->path_expr : std::vector<std::string>();
            for (const auto& p : _extra_paths) raw.push_back(p);
            append_system_exprs(raw);
            std::string hint, temp;
            if (!build_hint(std::string(), raw, hint, temp, _err)) {
                core::destroy(eng);
                return false;
            }
            const alx::bytes bin = eng->compile(_file, _cmps, _embed, hint);
            drop_hint(temp);
            core::destroy(eng);
            if (bin.empty()) {
                _err = cmpl_err.empty() ? "compile failed: " + _file : cmpl_err;
                return false;
            }
            if (!ensure_parent(_out, _err)) return false;
            if (!alx::file::write_all(_out, bin.data(), bin.size(), false)) {
                _err = "cannot write " + _out;
                return false;
            }
            return true;
        }

        // ── .axp read-only view (T10) ─────────────────────────────────────

        bool unpack_axp(const std::string& _path, axp_view& _out, std::string& _err) {
            const alx::bytes data = alx::file::read_all(_path);
            if (data.empty()) {
                _err = "cannot read " + _path;
                return false;
            }
            alx::varmap raw;
            if (!alx::script::engine::unpack(alx::bytes_view(data), raw)) {
                _err = "not a compiled binary: " + _path;
                return false;
            }
            _out = axp_view();
            _out.main_text = raw.value("ast").to<std::string>();
            if (raw.contain("modules")) {
                for (const auto& kv : raw.value("modules").to<alx::varmap>()) // kv.second: variant*
                    _out.modules.emplace_back(kv.first, kv.second->to<std::string>());
            }
            // info is metadata → serialize as pretty JSON (GUI shows it as JSON)
            _out.info_text = alx::json_doc::to_string(
                alx::json_value(alx::json_object::from_varmap(raw.value("info").to<alx::varmap>())),
                false);
            if (raw.contain("hint")) {
                _out.has_hint = true;
                _out.hint_text = raw.value("hint").to<std::string>();
            }
            return true;
        }

        std::vector<std::string> axp_search_paths(const std::string& _axp, std::string& _err) {
            axp_view v;
            if (!unpack_axp(_axp, v, _err)) return {};
            if (!v.has_hint) return {};
            bool ok = false;
            const alx::json_value jv = alx::json_doc::from_value(v.hint_text, &ok);
            if (!ok || !jv.is<alx::json_object>()) {
                _err = "hint is not a JSON object: " + _axp;
                return {};
            }
            const alx::json_object& o = jv.to<alx::json_object>();
            std::vector<std::string> out;
            if (o.contain("search_paths") && o.value("search_paths").is<alx::json_array>())
                for (const alx::json_value* e : o.value("search_paths").to<alx::json_array>())
                    if (e->is<std::string>()) out.push_back(e->to<std::string>());
            return out;
        }

    } // namespace project
} // namespace core
