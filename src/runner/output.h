#ifndef ARUNNER_OUTPUT_H
#define ARUNNER_OUTPUT_H

#include <acomm_ex.h>
#include <avariant.h>

#include "../proto/config.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

// writer — the only data_send caller: control first, events in enqueue order.
// output() is the only aggregate channel; every other message closes the open frame.
namespace runner {
    // frame_options — display shaping and the session log path, handed over by the GUI
    struct frame_options {
        alx::uint_64 frame_ms = proto::config::kFrameMsDefault; // 0 = no time framing
        alx::uint_64 frame_bytes = proto::config::kFrameBytesDefault; // 0 = no size cap
        alx::uint_64 trim_bytes = proto::config::kTrimBytesDefault; // 0 = no trimming
        std::string log_path; // empty = no log file
    };

    class writer {
    public:
        writer(alx::comm_ex& _comm, frame_options _opts = frame_options(),
               alx::uint_64 _high_water = 8u * 1024u * 1024u);
        ~writer();

        void start();
        void stop(bool _drain);

        // send_control — small host answer, jumps ahead of queued events
        void send_control(alx::varmap&& _m);
        // send_event — protocol event; closes the open frame, stamps lastOutputSeq, keeps order
        void send_event(alx::varmap&& _m, bool _compress = false);
        // output — engine-thread bytes: log first, then trim, then the open frame
        void output(const std::string& _data);
        // interrupt — release producers blocked on a full frame; the pending write is dropped
        void interrupt();
        // wake — notify the loop and the producers
        void wake();
        // sync_log — push buffered log bytes to the OS; call when a run ends
        void sync_log();

        // set_trace — called with every sent varmap, on the writer thread
        void set_trace(std::function<void(const alx::varmap&)> _fn) { m_trace = std::move(_fn); }

    private:
        struct item {
            alx::varmap m;
            alx::uint_64 weight = 0;
            bool compress = false;
        };

        bool framed() const { return m_opts.frame_ms || m_opts.frame_bytes; }

        void loop();
        void open_log();
        const char* log_note() const;
        alx::uint_64 append_framed(const std::string& _data);
        void output_chunked(const std::string& _data);
        void close_frame_locked();
        void wait_room_locked(std::unique_lock<std::mutex>& _lk);
        void stamp_last_seq_locked(alx::varmap& _m);

    private:
        alx::comm_ex& m_comm;
        frame_options m_opts;
        alx::uint_64 m_high_water;

        std::thread m_thread;
        std::mutex m_mtx;
        std::condition_variable m_cv;
        std::deque<item> m_control;
        std::deque<item> m_events;
        alx::uint_64 m_pending = 0;
        alx::uint_64 m_seq = 0;

        std::string m_frame;
        bool m_deadline_set = false;
        std::chrono::steady_clock::time_point m_deadline;

        std::mutex m_log_mtx;
        std::FILE* m_log = nullptr;
        bool m_log_failed = false;
        alx::uint_64 m_log_bytes = 0;
        alx::uint_64 m_log_seq = 0; // last frame seq flushed to the log

        std::atomic<bool> m_stopping{false};
        std::atomic<bool> m_abort{false};
        std::function<void(const alx::varmap&)> m_trace;
    };
} // namespace runner

#endif // ARUNNER_OUTPUT_H
