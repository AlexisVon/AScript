#include "output.h"

#include "../proto/message.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace runner {
    namespace {
        constexpr size_t kChunkBytes = 64u * 1024u;

        // human_bytes — size of the omitted middle for the trim marker
        std::string human_bytes(size_t _n) {
            char b[64];
            if (_n >= 1024u * 1024u)
                std::snprintf(b, sizeof(b), "%.2f MiB", _n / (1024.0 * 1024.0));
            else if (_n >= 1024u)
                std::snprintf(b, sizeof(b), "%.1f KiB", _n / 1024.0);
            else
                std::snprintf(b, sizeof(b), "%zu B", _n);
            return std::string(b);
        }

        // utf8_floor / utf8_ceil — keep a cut outside a multi-byte sequence
        size_t utf8_floor(const std::string& _s, size_t _pos) {
            while (_pos > 0 && _pos < _s.size() && (static_cast<unsigned char>(_s[_pos]) & 0xC0) == 0x80) --_pos;
            return _pos;
        }

        size_t utf8_ceil(const std::string& _s, size_t _pos) {
            while (_pos < _s.size() && (static_cast<unsigned char>(_s[_pos]) & 0xC0) == 0x80) ++_pos;
            return _pos;
        }

        // head_cut — end of the last line inside the backoff window
        size_t head_cut(const std::string& _s, size_t _want, size_t _window) {
            const size_t lo = _want > _window ? _want - _window : 0;
            for (size_t p = _want; p > lo; --p)
                if (_s[p - 1] == '\n') return p;
            return utf8_floor(_s, _want);
        }

        // tail_cut — start of the first line inside the backoff window
        size_t tail_cut(const std::string& _s, size_t _want, size_t _window) {
            const size_t hi = std::min(_s.size(), _want + _window);
            for (size_t p = _want; p < hi; ++p)
                if (_s[p] == '\n') return p + 1;
            return utf8_ceil(_s, _want);
        }

        // trim_line — head + marker + tail; false when the data fits or the windows overlap
        bool trim_line(const std::string& _data, size_t _k, const char* _note, std::string& _out) {
            if (_k == 0 || _data.size() <= _k) return false;
            const size_t half = _k / 2;
            const size_t window = std::min<size_t>(1024u, std::max<size_t>(1u, _k / 4));
            const size_t head = head_cut(_data, half, window);
            const size_t tail = tail_cut(_data, _data.size() - half, window);
            if (tail <= head) return false;
            size_t lines = 0;
            const char* p = _data.data() + head;
            const char* end = _data.data() + tail;
            while ((p = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(end - p)))) != nullptr) {
                ++lines;
                ++p;
            }
            const std::string mid = human_bytes(tail - head);
            char info[192];
            if (lines)
                std::snprintf(info, sizeof(info), "[… 已省略 %s / %zu 行，%s …]", mid.c_str(), lines, _note);
            else
                std::snprintf(info, sizeof(info), "[… 已省略 %s，%s …]", mid.c_str(), _note);
            _out.assign(_data.data(), head);
            _out += "\n...\n";
            _out += info;
            _out += "\n...\n";
            _out.append(_data, tail, std::string::npos);
            return true;
        }
    } // namespace

    writer::writer(alx::comm_ex& _comm, frame_options _opts, alx::uint_64 _high_water)
        : m_comm(_comm), m_opts(std::move(_opts)), m_high_water(_high_water) {}

    writer::~writer() {
        stop(false);
    }

    void writer::start() {
        if (m_thread.joinable()) return;
        m_thread = std::thread([this] { loop(); });
    }

    void writer::stop(bool _drain) {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_stopping.store(true);
            if (_drain) {
                close_frame_locked();
            } else {
                m_control.clear();
                m_events.clear();
                m_frame.clear();
                m_deadline_set = false;
                m_pending = 0;
            }
        }
        m_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();
        std::lock_guard<std::mutex> lk(m_log_mtx);
        if (m_log) {
            std::fclose(m_log);
            m_log = nullptr;
            if (m_log_bytes == 0) {
                std::error_code ec;
                std::filesystem::remove(std::filesystem::u8path(m_opts.log_path), ec);
            }
        }
    }

    void writer::send_control(alx::varmap&& _m) {
        item it;
        it.m = std::move(_m);
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (m_stopping.load()) return;
            close_frame_locked();
            m_control.push_back(std::move(it));
        }
        m_cv.notify_all();
    }

    void writer::send_event(alx::varmap&& _m, bool _compress) {
        item it;
        it.m = std::move(_m);
        it.compress = _compress;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (m_stopping.load()) return;
            close_frame_locked();
            stamp_last_seq_locked(it.m);
            m_events.push_back(std::move(it));
        }
        m_cv.notify_all();
    }

    void writer::output(const std::string& _data) {
        if (_data.empty() || m_stopping.load()) return;
        bool failed = false;
        std::string note;
        {
            std::lock_guard<std::mutex> lk(m_log_mtx);
            if (!m_opts.log_path.empty() && !m_log && !m_log_failed) {
                open_log();
                failed = m_log_failed;
            }
            if (m_log) {
                if (std::fwrite(_data.data(), 1, _data.size(), m_log) == _data.size())
                    m_log_bytes += _data.size();
                else
                    m_log_failed = true;
            }
            note = log_note();
        }
        if (failed) send_event(proto::make_log("warn", "cannot write session log: " + m_opts.log_path));
        std::string trimmed;
        const std::string* data = &_data;
        if (trim_line(_data, m_opts.trim_bytes, note.c_str(), trimmed)) data = &trimmed;
        if (!framed()) {
            output_chunked(*data);
            return;
        }
        const alx::uint_64 seq = append_framed(*data);
        // one flush per closed frame: a crash loses at most the open frame, not the buffered history
        std::lock_guard<std::mutex> lk(m_log_mtx);
        if (seq != m_log_seq) {
            m_log_seq = seq;
            if (m_log && std::fflush(m_log) != 0) m_log_failed = true;
        }
    }

    void writer::interrupt() {
        m_abort.store(true);
        m_cv.notify_all();
    }

    void writer::wake() {
        m_cv.notify_all();
    }

    void writer::sync_log() {
        std::lock_guard<std::mutex> lk(m_log_mtx);
        if (m_log && std::fflush(m_log) != 0) m_log_failed = true;
    }

    void writer::open_log() {
        if (m_opts.log_path.empty()) return;
        const std::filesystem::path path = std::filesystem::u8path(m_opts.log_path);
#ifdef _WIN32
#    ifdef _MSC_VER
        if (::_wfopen_s(&m_log, path.c_str(), L"wb") != 0) m_log = nullptr;
#    else
        m_log = ::_wfopen(path.c_str(), L"wb");
#    endif
#else
        m_log = std::fopen(path.c_str(), "wb");
#endif
        if (!m_log) {
            m_log_failed = true;
            return;
        }
        std::setvbuf(m_log, nullptr, _IOFBF, 64u * 1024u);
    }

    const char* writer::log_note() const {
        if (m_opts.log_path.empty()) return "完整内容未保存";
        if (m_log_failed || !m_log) return "会话日志写入失败";
        return "完整内容见会话日志";
    }

    alx::uint_64 writer::append_framed(const std::string& _data) {
        std::unique_lock<std::mutex> lk(m_mtx);
        const size_t cap = m_opts.frame_bytes;
        if (cap && !m_frame.empty() && m_frame.size() + _data.size() > cap) {
            if (m_opts.frame_ms == 0) {
                // no deadline to wait for: close the full frame now (T=0 mode)
                wait_room_locked(lk);
                if (m_stopping.load() || m_abort.load()) return m_seq;
                close_frame_locked();
            } else {
                m_cv.wait(lk, [this] { return m_frame.empty() || m_stopping.load() || m_abort.load(); });
            }
        }
        if (m_stopping.load() || m_abort.load()) return m_seq;
        if (m_frame.empty()) {
            m_deadline_set = m_opts.frame_ms > 0;
            if (m_deadline_set)
                m_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(m_opts.frame_ms);
        }
        m_frame.append(_data);
        // a size-only frame closes as soon as it is full; a no-cap frame closes at the high water
        if (m_opts.frame_ms == 0 && cap && m_frame.size() >= cap) {
            wait_room_locked(lk);
            if (!m_stopping.load() && !m_abort.load()) close_frame_locked();
        } else if (m_opts.frame_ms > 0 && !cap && m_frame.size() >= m_high_water) {
            wait_room_locked(lk);
            if (!m_stopping.load() && !m_abort.load()) close_frame_locked();
        }
        m_cv.notify_all();
        return m_seq;
    }

    void writer::output_chunked(const std::string& _data) {
        for (size_t off = 0; off < _data.size(); off += kChunkBytes) {
            const size_t n = std::min(kChunkBytes, _data.size() - off);
            {
                std::unique_lock<std::mutex> lk(m_mtx);
                wait_room_locked(lk);
                if (m_stopping.load() || m_abort.load()) return;
                item it;
                it.m = proto::make_output(
                    "stdout", ++m_seq,
                    alx::bytes(reinterpret_cast<const void*>(_data.data() + off), n));
                it.weight = n;
                m_pending += n;
                m_events.push_back(std::move(it));
            }
            m_cv.notify_all();
        }
    }

    void writer::close_frame_locked() {
        if (m_frame.empty()) {
            m_deadline_set = false;
            return;
        }
        item it;
        it.weight = m_frame.size();
        it.m = proto::make_output("stdout", ++m_seq,
                                  alx::bytes(reinterpret_cast<const void*>(m_frame.data()), m_frame.size()));
        m_pending += it.weight;
        m_frame.clear();
        m_deadline_set = false;
        m_events.push_back(std::move(it));
        m_cv.notify_all();
    }

    void writer::wait_room_locked(std::unique_lock<std::mutex>& _lk) {
        m_cv.wait(_lk, [this] {
            return m_pending <= m_high_water || m_stopping.load() || m_abort.load();
        });
    }

    void writer::stamp_last_seq_locked(alx::varmap& _m) {
        if (!_m.contain(proto::kPayloadKey)) return;
        alx::varmap fallback;
        alx::varmap& p = _m[proto::kPayloadKey].to(fallback);
        if (!p.contain("lastOutputSeq")) return;
        p["lastOutputSeq"] = alx::variant(alx::int_64(m_seq));
    }

    void writer::loop() {
        for (;;) {
            item it;
            bool empty = false;
            {
                std::unique_lock<std::mutex> lk(m_mtx);
                for (;;) {
                    if (!m_control.empty() || !m_events.empty()) break;
                    if (m_deadline_set && std::chrono::steady_clock::now() >= m_deadline) {
                        if (m_pending <= m_high_water) {
                            close_frame_locked();
                            continue;
                        }
                        m_cv.wait(lk); // room frees up, then re-check
                        continue;
                    }
                    if (m_stopping.load()) {
                        empty = true;
                        break;
                    }
                    if (m_deadline_set)
                        m_cv.wait_until(lk, m_deadline);
                    else
                        m_cv.wait(lk);
                }
                if (empty) break;
                if (!m_control.empty()) {
                    it = std::move(m_control.front());
                    m_control.pop_front();
                } else {
                    it = std::move(m_events.front());
                    m_events.pop_front();
                }
            }
            if (m_trace) m_trace(it.m);
            m_comm.data_send(it.m, 0, it.compress);
            if (it.weight) {
                std::lock_guard<std::mutex> lk(m_mtx);
                m_pending -= it.weight;
            }
            m_cv.notify_all();
        }
    }

} // namespace runner
