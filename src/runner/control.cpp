#include "control.h"

#include <chrono>

namespace runner {
    namespace {
        // takes — whether _m belongs to _thread; continue is global and step is main-only
        bool takes(const proto::message& _m, alx::uint_64 _thread) {
            if (_m.type == proto::kContinue) return true;
            if (_m.type == proto::kStep) return _thread == 0;
            if (_m.type == proto::kSnapshot || _m.type == proto::kView) {
                if (!_m.payload.contain("thread")) return _thread == 0;
                return static_cast<alx::uint_64>(_m.payload.value("thread").to<alx::int_64>(0)) == _thread;
            }
            return _thread == 0;
        }
    } // namespace

    void control::request_stop() {
        m_stop.store(true);
        if (auto* e = m_engine.load()) e->set_interrupt();
        m_cv.notify_all();
        m_ecv.notify_all();
        m_dcv.notify_all();
    }

    void control::request_shutdown() {
        m_shutdown.store(true);
        request_stop();
        m_ecv.notify_all();
    }

    void control::push(const alx::varmap& _m) {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_inbox.push_back(_m);
        }
        m_cv.notify_all();
    }

    bool control::pop(alx::varmap& _out, alx::uint_32 _mill) {
        std::unique_lock<std::mutex> lk(m_mtx);
        m_cv.wait_for(lk, std::chrono::milliseconds(_mill), [this] { return !m_inbox.empty(); });
        if (m_inbox.empty()) return false;
        _out = std::move(m_inbox.front());
        m_inbox.pop_front();
        return true;
    }

    void control::wake() {
        m_cv.notify_all();
        m_ecv.notify_all();
    }

    bool control::queue_exec(proto::exec_request _req, alx::int_64 _id) {
        // console lines may arrive while the previous one is settling; the engine thread serializes
        if (m_busy.load() && !_req.console) return false;
        {
            std::lock_guard<std::mutex> lk(m_emtx);
            m_execs.emplace_back(std::move(_req), _id);
        }
        m_busy.store(true);
        m_done.store(false);
        m_ecv.notify_all();
        return true;
    }

    bool control::take_exec(proto::exec_request& _req, alx::int_64& _id) {
        std::unique_lock<std::mutex> lk(m_emtx);
        m_ecv.wait(lk, [this] { return !m_execs.empty() || m_shutdown.load(); });
        if (m_execs.empty()) return false;
        _req = std::move(m_execs.front().first);
        _id = m_execs.front().second;
        m_execs.pop_front();
        return true;
    }

    void control::mark_exec_done(bool _finished) {
        m_busy.store(false);
        m_done.store(_finished);
        m_ecv.notify_all();
    }

    void control::push_debug(proto::message _m) {
        {
            std::lock_guard<std::mutex> lk(m_dmtx);
            m_debug.push_back(std::move(_m));
        }
        m_dcv.notify_all();
    }

    bool control::pop_debug(proto::message& _out, alx::uint_32 _mill) {
        std::unique_lock<std::mutex> lk(m_dmtx);
        m_dcv.wait_for(lk, std::chrono::milliseconds(_mill), [this] { return !m_debug.empty(); });
        if (m_debug.empty()) return false;
        _out = std::move(m_debug.front());
        m_debug.pop_front();
        return true;
    }

    bool control::pop_debug_for(alx::uint_64 _thread, proto::message& _out, alx::uint_32 _mill) {
        std::unique_lock<std::mutex> lk(m_dmtx);
        m_dcv.wait_for(lk, std::chrono::milliseconds(_mill), [this, _thread] {
            for (const auto& m : m_debug)
                if (takes(m, _thread)) return true;
            return false;
        });
        for (auto it = m_debug.begin(); it != m_debug.end(); ++it) {
            if (!takes(*it, _thread)) continue;
            _out = std::move(*it);
            m_debug.erase(it);
            return true;
        }
        return false;
    }

    void control::clear_debug() {
        std::lock_guard<std::mutex> lk(m_dmtx);
        m_debug.clear();
    }

    void control::push_breakpoints(proto::message _m) {
        std::lock_guard<std::mutex> lk(m_bmtx);
        m_bp_queue.push_back(std::move(_m));
        m_bp_ready.store(true, std::memory_order_release);
    }

    bool control::take_breakpoints(proto::message& _out) {
        if (!m_bp_ready.load(std::memory_order_acquire)) return false;
        std::lock_guard<std::mutex> lk(m_bmtx);
        if (m_bp_queue.empty()) {
            m_bp_ready.store(false, std::memory_order_release);
            return false;
        }
        _out = std::move(m_bp_queue.front());
        m_bp_queue.pop_front();
        if (m_bp_queue.empty()) m_bp_ready.store(false, std::memory_order_release);
        return true;
    }

    void control::push_input(std::string _text) {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_input.push_back(std::move(_text));
        }
        m_cv.notify_all();
    }

    std::string control::wait_input() {
        std::unique_lock<std::mutex> lk(m_mtx);
        m_cv.wait(lk, [this] { return !m_input.empty() || m_stop.load() || m_shutdown.load(); });
        if (m_input.empty()) return std::string();
        std::string text = std::move(m_input.front());
        m_input.pop_front();
        return text;
    }

} // namespace runner
