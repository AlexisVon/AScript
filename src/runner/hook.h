#ifndef ARUNNER_HOOK_H
#define ARUNNER_HOOK_H

#include <ascript.h>

#include "control.h"
#include "output.h"
#include "source_registry.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// hook — breakpoints, stepping, trap and pause, served on every engine thread
namespace runner {
    class hook {
    public:
        hook(control& _ctl, writer& _out, source_registry& _src);

        // configure — breakpoints and options for one run; resets the stop wave
        void configure(const proto::breakpoint_list& _bps, bool _stop_on_entry, bool _skip_traps,
                       bool _multi_thread);
        // finish — release parked threads; _console keeps its children alive between execs
        void finish(bool _console);
        // shutdown — interrupt and wake every thread before the engine goes away
        void shutdown();

        // child_observer — core::inspect child hook; installs the park-only hook
        static void child_observer(void* _ud, bool _attach, alx::script::engine* _child,
                                   std::thread::id _parent, std::thread::id _child_tid);
        // main_ud — the hook cookie of the session's main engine
        void* main_ud() { return &m_main; }

        // entry — engine hook_fn thunk; hkdt is the thread context
        static bool entry(alx::script::hook_info& _info);
        bool dispatch(alx::script::hook_info& _info);

    private:
        // thread_ctx — hook_ud of one engine; id 0 is the session's main engine
        struct thread_ctx {
            hook* owner = nullptr;
            alx::uint_64 id = 0;
            alx::int_64 parent = -1;
            std::thread::id tid;
        };

        bool dispatch_main(alx::script::hook_info& _info);
        bool dispatch_child(alx::script::hook_info& _info, thread_ctx& _ctx);
        bool on_exec(alx::script::hook_info& _info);
        bool on_debug(alx::script::hook_info& _info);
        bool on_trap(alx::script::hook_info& _info);
        // stop_wave — report this thread's stop point, then park it
        bool stop_wave(alx::script::hook_info& _info, thread_ctx& _ctx, const char* _reason,
                       std::string _label = std::string(), std::string _trap_args = std::string());
        bool park(alx::script::hook_info& _info, thread_ctx& _ctx);
        alx::uint_64 resume_all();
        void observe(bool _attach, alx::script::engine* _child, std::thread::id _parent,
                     std::thread::id _child_tid);
        void serve_snapshot(alx::script::hook_info& _info, const proto::message& _cmd);
        void serve_view(alx::script::hook_info& _info, const proto::message& _cmd);
        void apply_pending_breakpoints();
        void apply_breakpoints(const proto::message& _cmd);

        const void* current_entity(alx::script::hook_info& _info) const;
        // breakpoint_at — the matching breakpoint, or null; its log may be empty (pause)
        const proto::breakpoint* breakpoint_at(alx::script::hook_info& _info, const void* _entity,
                                               alx::uint_64 _line);
        alx::uint_64 func_depth(alx::script::hook_info& _info) const;
        std::vector<proto::stack_frame> frames(alx::script::hook_info& _info, alx::uint_64 _row,
                                               alx::uint_64 _col, bool _child);

    private:
        // entity breakpoint view — rebuilt when breakpoints change
        struct bp_view {
            std::string source;
            std::unordered_map<alx::uint_64, proto::breakpoint> lines;
        };

        control& m_ctl;
        writer& m_out;
        source_registry& m_src;
        proto::breakpoint_list m_bps;
        std::unordered_map<const void*, bp_view> m_bp_views;

        // main-thread debug state
        enum class step_mode { none,
                               in,
                               over,
                               out };
        step_mode m_step = step_mode::none;
        alx::uint_64 m_step_depth = 0;
        alx::uint_64 m_step_epoch = 0;
        bool m_stop_on_entry = false;
        std::atomic<bool> m_skip_traps{false}; // read by child threads
        std::atomic<bool> m_multi_thread{false}; // read by child threads

        // stop wave and child registry, shared by every engine thread
        thread_ctx m_main;
        std::mutex m_mtx;
        std::condition_variable m_cv;
        std::unordered_map<alx::uint_64, thread_ctx*> m_children;
        std::unordered_map<std::thread::id, alx::uint_64> m_tids;
        alx::uint_64 m_next_id = 1;
        alx::uint_64 m_stop_seq = 0;
        alx::uint_64 m_trigger = 0;
        std::atomic<bool> m_run_active{false};
        std::atomic<bool> m_exit{false};
        std::atomic<bool> m_stopped{false};
        std::atomic<alx::uint_64> m_epoch{0};
    };
} // namespace runner

#endif // ARUNNER_HOOK_H
