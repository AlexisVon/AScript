#include "thd.h"
#include "../core.h"
#include "../inspect.h"

#include <thread>

// $thd_xxx extensions — thread execution with isolated engine instances
namespace core {
    namespace thd_impl {

        static void thd_worker(thd_handle* _h, std::string _source,
                               std::string _home_dir, alx::varmap _params,
                               alx::script::engine_config _cfg, std::thread::id _parent) {
            // the child is not the debug session: the parent's hook must not run on this thread
            _cfg.hook_fn_ptr = nullptr;
            _cfg.hook_ud = nullptr;
            _cfg.hook_interval = 0;
            _h->eng = core::create(_cfg);
            const std::thread::id child = std::this_thread::get_id();
            inspect::notify_child(true, _h->eng, _parent, child);

            // write params into child root scope
            for (auto it = _params.begin(); it != _params.end(); ++it) {
                auto* slot = _h->eng->load(it.key(), true);
                if (slot) *slot = it.value();
            }

            alx::bytes src(_source);
            _h->res = _h->eng->exec(alx::bytes_view(src), _home_dir);
            core::destroy(_h->eng);
            inspect::notify_child(false, _h->eng, _parent, child);
            _h->eng = nullptr;
            _h->done.store(true, std::memory_order_release);
        }

        static void ext_create(alx::script::fwrap& fw) {
            std::string source = core::utils::as_str(fw[0], "thd_create source");

            alx::varmap params;
            if (fw.size() > 1 && fw[1].is<alx::varmap>())
                params = fw[1].to<alx::varmap>();

            std::string home_dir;
            if (fw.size() > 2 && fw[2].is<std::string>())
                home_dir = fw[2].as<std::string>();

            // inherit parent engine config
            auto child_cfg = fw.config();

            const std::thread::id parent = std::this_thread::get_id();
            auto* h = new thd_handle();
            h->thr = std::thread(thd_worker, h, source, home_dir,
                                 std::move(params), std::move(child_cfg), parent);
            fw.freturn(alx::variant(alx::anyptr_ex<thd_handle>::make_ref(h)));
        }

        static void ext_join(alx::script::fwrap& fw) {
            auto* h = utils::as_handle<thd_handle>(fw[0], "thd_join: handle from $thd_create");
            if (h->thr.joinable()) h->thr.join();
            fw.freturn(h->res.value);
        }

        static void ext_running(alx::script::fwrap& fw) {
            auto* h = utils::as_handle<thd_handle>(fw[0], "thd_running: handle from $thd_create");
            fw.freturn(alx::variant(!h->done.load(std::memory_order_acquire)));
        }

        static std::string thread_desc(const alx::anyptr& _p) {
            auto* h = alx::anyptr_ex<thd_handle>::as(_p);
            if (!h) return std::string();
            return h->done.load(std::memory_order_acquire) ? "finished" : "running";
        }

        static alx::variant thread_view(const alx::anyptr& _p) {
            auto* h = alx::anyptr_ex<thd_handle>::as(_p);
            if (!h) return alx::variant();
            const bool done = h->done.load(std::memory_order_acquire);
            std::string text = done ? "state: finished\n" : "state: running\n";
            if (done) {
                text += "error: ";
                text += alx::script::error_type_name(h->res.error);
                text += "\n";
                if (!h->res.value.null()) {
                    text += "value: ";
                    text += core::utils::display(h->res.value);
                    text += "\n";
                }
            }
            alx::varmap m;
            m["kind"] = alx::variant(std::string("text"));
            m["format"] = alx::variant(std::string("plain"));
            m["data"] = alx::variant(alx::bytes(text.data(), text.size()));
            return alx::variant(std::move(m));
        }

    } // namespace thd_impl

    void thd::install(alx::script::engine* _e) const {
        inspect::add(alx::anyptr_ex<thd_handle>::rid, "thread", thd_impl::thread_desc,
                     thd_impl::thread_view);
        _e->set_extend("thd_create", thd_impl::ext_create);
        _e->set_extend("thd_join", thd_impl::ext_join);
        _e->set_extend("thd_running", thd_impl::ext_running);
    }

    const char* thd::doc_path() const {
        return "thread.json";
    }

} // namespace core
