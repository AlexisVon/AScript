#ifndef ARUNNER_WATCHDOG_H
#define ARUNNER_WATCHDOG_H

#include <ascript.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

// watchdog — wall-clock budget: set_interrupt() once it passes
namespace runner {
    class watchdog {
    public:
        watchdog(alx::script::engine* _e, alx::uint_32 _wall_ms);
        ~watchdog();

        void start();
        void stop();
        bool fired() const { return m_fired.load(); }

    private:
        void loop();

    private:
        alx::script::engine* m_engine;
        alx::uint_32 m_wall_ms;
        std::thread m_thread;
        std::mutex m_mtx;
        std::condition_variable m_cv;
        std::atomic<bool> m_done{false};
        std::atomic<bool> m_fired{false};
    };
} // namespace runner

#endif // ARUNNER_WATCHDOG_H
