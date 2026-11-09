#ifndef ARUNNER_CONTROL_H
#define ARUNNER_CONTROL_H

#include <ascript.h>
#include <avariant.h>

#include "../proto/message.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

// control — shared state between the comm callback, the control loop and the engine thread
namespace runner {
    class control {
    public:
        // stop / interrupt — callable from any thread
        void request_stop();
        bool stop_requested() const { return m_stop.load(); }
        void request_shutdown();
        bool shutdown_requested() const { return m_shutdown.load(); }

        // link_lost — the transport dropped; closing comm is what releases blocked senders
        void set_link_lost(bool _on) { m_link_lost.store(_on); }
        bool link_lost() const { return m_link_lost.load(); }

        // attach_engine — the engine the control thread may interrupt; null when idle
        void attach_engine(alx::script::engine* _e) { m_engine.store(_e); }
        void detach_engine() { m_engine.store(nullptr); }

        // push — hand a received varmap to the control loop (comm worker thread)
        void push(const alx::varmap& _m);
        bool pop(alx::varmap& _out, alx::uint_32 _mill);
        void wake();

        // queue_exec — hand one run to the engine thread; false when one is already busy
        bool queue_exec(proto::exec_request _req, alx::int_64 _id);
        bool take_exec(proto::exec_request& _req, alx::int_64& _id);
        void mark_exec_done(bool _finished);
        // a run is over; the next exec may be accepted before the message is flushed
        void mark_idle() { m_busy.store(false); }
        // reset — drop the engine before the next console exec
        void request_reset() { m_reset.store(true); }
        bool consume_reset() { return m_reset.exchange(false); }
        bool exec_done() const { return m_done.load(); }
        bool busy() const { return m_busy.load(); }

        // pause request — the hook stops at the next statement
        void request_pause() { m_pause.store(true); }
        bool pause_requested() const { return m_pause.load(); }
        void clear_pause() { m_pause.store(false); }

        // debug commands — consumed by the pause loops; continue is global, the rest are targeted
        void push_debug(proto::message _m);
        bool pop_debug(proto::message& _out, alx::uint_32 _mill);
        bool pop_debug_for(alx::uint_64 _thread, proto::message& _out, alx::uint_32 _mill);
        void clear_debug();
        void set_debug_active(bool _on) { m_debug_active.store(_on); }
        bool debug_active() const { return m_debug_active.load(); }

        // breakpoint updates — queued by the control loop, applied by the hook on the engine thread
        void push_breakpoints(proto::message _m);
        bool take_breakpoints(proto::message& _out);

        // push_input / wait_input — the engine thread's pipe_in, answered by the control loop
        void push_input(std::string _text);
        std::string wait_input();

        // exit_code — what the run's result asked the process to return
        void set_exit_code(int _code) { m_exit_code.store(_code); }
        int exit_code() const { return m_exit_code.load(); }

    private:
        std::mutex m_mtx;
        std::condition_variable m_cv;
        std::deque<alx::varmap> m_inbox;
        std::deque<std::string> m_input;

        std::mutex m_dmtx;
        std::condition_variable m_dcv;
        std::deque<proto::message> m_debug;

        std::mutex m_bmtx;
        std::deque<proto::message> m_bp_queue;
        std::atomic<bool> m_bp_ready{false};

        std::mutex m_emtx;
        std::condition_variable m_ecv;
        std::deque<std::pair<proto::exec_request, alx::int_64>> m_execs;

        std::atomic<alx::script::engine*> m_engine{nullptr};
        std::atomic<bool> m_stop{false};
        std::atomic<bool> m_shutdown{false};
        std::atomic<bool> m_pause{false};
        std::atomic<bool> m_debug_active{false};
        std::atomic<bool> m_link_lost{false};
        std::atomic<bool> m_busy{false};
        std::atomic<bool> m_done{false};
        std::atomic<bool> m_reset{false};
        std::atomic<int> m_exit_code{0};
    };
} // namespace runner

#endif // ARUNNER_CONTROL_H
