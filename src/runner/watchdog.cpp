#include "watchdog.h"

#include <chrono>

namespace runner {

    watchdog::watchdog(alx::script::engine* _e, alx::uint_32 _wall_ms)
        : m_engine(_e), m_wall_ms(_wall_ms) {}

    watchdog::~watchdog() {
        stop();
    }

    void watchdog::start() {
        if (!m_wall_ms || m_thread.joinable()) return;
        m_thread = std::thread([this] { loop(); });
    }

    void watchdog::stop() {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_done.store(true);
        }
        m_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }

    void watchdog::loop() {
        std::unique_lock<std::mutex> lk(m_mtx);
        if (m_cv.wait_for(lk, std::chrono::milliseconds(m_wall_ms), [this] { return m_done.load(); }))
            return;
        if (m_engine && m_engine->is_running()) {
            m_fired.store(true);
            m_engine->set_interrupt();
        }
    }

} // namespace runner
