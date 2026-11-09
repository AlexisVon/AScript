#ifndef ARUNNER_SESSION_H
#define ARUNNER_SESSION_H

#include <ascript.h>
#include <avariant.h>

#include "../proto/message.h"
#include "control.h"
#include "hook.h"
#include "output.h"
#include "source_registry.h"

#include <string>
#include <vector>

// session — one engine, one run: create, wire the pipes and hook, exec, report
namespace runner {
    class session {
    public:
        session(control& _ctl, writer& _out) : m_ctl(_ctl), m_out(_out), m_src(), m_hook(_ctl, _out, m_src) {}
        ~session();

        // run — execute one request on the calling (engine) thread; never throws
        void run(const proto::exec_request& _req);

    private:
        // resolve — build the engine config, main path and search paths for _req
        bool resolve(const proto::exec_request& _req, alx::varmap& _cfg, std::string& _main,
                     std::vector<std::string>& _paths, std::string& _err);
        // finish — send result + exited, then drop the engine
        void finish(bool _ok, const alx::script::engine::result& _res);
        // fail — report a pre-run failure as result + exited
        void fail(const std::string& _msg);

        static void pipe_out_cb(const std::string* _s, void* _ud);
        static std::string pipe_in_cb(void* _ud);

    private:
        control& m_ctl;
        writer& m_out;
        source_registry m_src;
        hook m_hook;
        alx::script::engine* m_engine = nullptr;
        bool m_console = false;
    };
} // namespace runner

#endif // ARUNNER_SESSION_H
