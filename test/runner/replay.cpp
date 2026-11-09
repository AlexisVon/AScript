// axc-replay — a headless GUI stand-in for the runner protocol tests.
// It listens like axc-gui, spawns axc-runner, speaks the handshake, and checks
// output ordering/losslessness, stop latency and compile errors.
#include "proto/config.h"
#include "proto/message.h"

#include <acomm_ex.h>
#include <aplatform.h>
#include <atransmit.h>
#include <avariant.h>
#include <avarsolid.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#    include <unistd.h>
#endif

namespace {
    int g_failures = 0;

    // check — one named expectation
    void check(bool _cond, const char* _what) {
        std::printf("  %s %s\n", _cond ? "ok  " : "FAIL", _what);
        if (!_cond) ++g_failures;
    }

    // str_field / int_field / bool_field — payload readers with defaults
    std::string str_field(const proto::message& _m, const char* _key) {
        if (!_m.payload.contain(_key)) return std::string();
        return _m.payload.value(_key).to<std::string>();
    }

    alx::int_64 int_field(const proto::message& _m, const char* _key) {
        if (!_m.payload.contain(_key)) return 0;
        return _m.payload.value(_key).to<alx::int_64>(0);
    }

    bool bool_field(const proto::message& _m, const char* _key) {
        if (!_m.payload.contain(_key)) return false;
        return _m.payload.value(_key).to<bool>(false);
    }

    // peer — the GUI half: a local_server plus a received-message queue
    class peer {
    public:
        explicit peer(const std::string& _endpoint) {
            auto* trans = alx::transmit::create("local_server", _endpoint);
            if (!trans) return;
            m_comm = new alx::comm_ex(trans);
            m_comm->data_recv.connect([this](const alx::varmap& _v, alx::uint_64) {
                // pause() stalls the receive thread itself, which is what makes a slow reader real
                {
                    std::unique_lock<std::mutex> lk(m_pause_mtx);
                    m_pause_cv.wait(lk, [this] { return !m_paused; });
                }
                proto::message m;
                if (!proto::parse(_v, m)) return;
                std::lock_guard<std::mutex> lk(m_mtx);
                m_inbox.push_back(std::move(m));
                m_cv.notify_all();
            });
            m_comm->mesg_prit.connect([](const std::string& _s) {
                std::fprintf(stderr, "  [gui] %s\n", _s.c_str());
            });
        }

        ~peer() {
            pause(false);
            if (m_comm) {
                m_comm->close();
                delete m_comm;
            }
        }

        bool open() { return m_comm && m_comm->open(); }

        // pause — stop consuming bytes, so the runner's output path blocks on backpressure
        void pause(bool _on) {
            {
                std::lock_guard<std::mutex> lk(m_pause_mtx);
                m_paused = _on;
            }
            m_pause_cv.notify_all();
        }

        bool send(alx::varmap _m) { return m_comm && m_comm->data_send(_m, 0, false); }

        bool pop(proto::message& _out, int _ms) {
            std::unique_lock<std::mutex> lk(m_mtx);
            m_cv.wait_for(lk, std::chrono::milliseconds(_ms), [this] { return !m_inbox.empty(); });
            if (m_inbox.empty()) return false;
            _out = std::move(m_inbox.front());
            m_inbox.pop_front();
            return true;
        }

    private:
        alx::comm_ex* m_comm = nullptr;
        std::mutex m_mtx;
        std::condition_variable m_cv;
        std::deque<proto::message> m_inbox;

        std::mutex m_pause_mtx;
        std::condition_variable m_pause_cv;
        bool m_paused = false;
    };

    // config_hex — the runner's single startup argument: one varsolid blob as hex.
    // Defaults are the legacy path (no framing, no trim) so byte-exact assertions hold;
    // frame-control scenarios pass their own values.
    std::string config_hex(const std::string& _endpoint, const std::string& _token,
                           alx::uint_64 _frame_ms = 0, alx::uint_64 _frame_bytes = 0,
                           alx::uint_64 _trim_bytes = 0, const std::string& _log_path = std::string()) {
        alx::varmap cfg;
        cfg[proto::config::kTransport] = alx::variant(std::string("local"));
        cfg[proto::config::kEndpoint] = alx::variant(_endpoint);
        cfg[proto::config::kToken] = alx::variant(_token);
        cfg[proto::config::kFrameMs] = alx::variant(alx::int_64(_frame_ms));
        cfg[proto::config::kFrameBytes] = alx::variant(alx::int_64(_frame_bytes));
        cfg[proto::config::kTrimBytes] = alx::variant(alx::int_64(_trim_bytes));
        if (!_log_path.empty()) cfg[proto::config::kLogPath] = alx::variant(_log_path);
        return alx::varsolid::to_bytes(cfg).to_hex();
    }

    // harness — one server, one runner process, one handshake
    struct harness {
        std::unique_ptr<peer> gui;
        alx::process_ctrl ctrl;
        alx::uint_64 frame_ms = 0;
        alx::uint_64 frame_bytes = 0;
        alx::uint_64 trim_bytes = 0;

        explicit harness(std::string _runner) : ctrl(std::move(_runner)) {}

        bool start(int _attempt) {
            for (int i = 0; i < 10; ++i) {
                const int port = 20000 + ((static_cast<int>(::getpid()) * 17 + _attempt * 977 + i * 131) % 20000);
                const std::string endpoint = "127.0.0.1:" + std::to_string(port);
                gui = std::make_unique<peer>(endpoint);
                if (!gui->open()) {
                    gui.reset();
                    continue;
                }
                ctrl.sig_errput.connect([](const std::string& _s) {
                    std::fprintf(stderr, "  [runner-err] %s", _s.c_str());
                });
                if (!ctrl.start({"--config", config_hex(endpoint, kToken, frame_ms, frame_bytes, trim_bytes)})) {
                    std::fprintf(stderr, "  cannot start runner\n");
                    return false;
                }

                proto::message hello;
                if (!gui->pop(hello, 5000)) {
                    std::fprintf(stderr, "  no hello from runner\n");
                    return false;
                }
                if (hello.type != proto::kHello) {
                    std::fprintf(stderr, "  first message was %s, not hello\n", hello.type.c_str());
                    return false;
                }
                if (str_field(hello, "token") != kToken) {
                    std::fprintf(stderr, "  hello token mismatch\n");
                    return false;
                }
                if (int_field(hello, "proto") != proto::kVersion) {
                    std::fprintf(stderr, "  hello proto mismatch\n");
                    return false;
                }
                gui->send(proto::make_welcome("replay", true));
                std::printf("  runner up: pid=%lld engine=%s\n", static_cast<long long>(int_field(hello, "pid")),
                            str_field(hello, "engineVersion").c_str());
                return true;
            }
            std::fprintf(stderr, "  no free endpoint\n");
            return false;
        }

        static constexpr const char* kToken = "0123456789abcdef0123456789abcdef";
    };

    // outcome — everything a scenario collected
    struct outcome {
        std::string output;
        std::vector<alx::uint_64> seqs;
        std::vector<size_t> packets;
        proto::message result;
        proto::message exited;
        bool got_result = false;
        bool got_exited = false;
        int compile_errors = 0;
    };

    // drain — consume messages until result + exited, answering inputRequest and stop
    outcome drain(harness& _h, int _timeout_ms, int _stop_after_ms = -1,
                  const std::string& _input = std::string()) {
        outcome o;
        const auto t0 = std::chrono::steady_clock::now();
        bool stop_sent = false;
        while (true) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - t0)
                                     .count();
            if (elapsed >= _timeout_ms) break;
            if (_stop_after_ms >= 0 && !stop_sent && elapsed >= _stop_after_ms) {
                _h.gui->send(proto::make_stop(false));
                stop_sent = true;
            }

            proto::message m;
            if (!_h.gui->pop(m, 20)) {
                if (o.got_result && o.got_exited) break;
                continue;
            }
            if (m.type == proto::kOutput) {
                if (m.payload.contain("data")) {
                    const alx::bytes& b = m.payload.value("data").to<alx::bytes>();
                    if (b.size()) o.output.append(reinterpret_cast<const char*>(b.data()), b.size());
                    o.packets.push_back(b.size());
                }
                o.seqs.push_back(static_cast<alx::uint_64>(int_field(m, "seq")));
            } else if (m.type == proto::kCompileError) {
                ++o.compile_errors;
                std::printf("  compile error: %s:%lld:%lld %s\n", str_field(m, "path").c_str(),
                            static_cast<long long>(int_field(m, "row")),
                            static_cast<long long>(int_field(m, "col")), str_field(m, "msg").c_str());
            } else if (m.type == proto::kResult) {
                o.result = m;
                o.got_result = true;
            } else if (m.type == proto::kExited) {
                o.exited = m;
                o.got_exited = true;
            } else if (m.type == proto::kInputRequest) {
                _h.gui->send(proto::make_input(_input));
            } else if (m.type == proto::kLog) {
                std::printf("  runner log [%s] %s\n", str_field(m, "level").c_str(), str_field(m, "text").c_str());
            }
            if (o.got_result && o.got_exited) break;
        }
        if (o.got_exited) _h.gui->send(proto::make_shutdown());
        return o;
    }

    // test_text — one print round trip
    void test_text(harness& _h) {
        std::printf("text: one print\n");
        proto::exec_request req;
        req.mode = "text";
        req.text = R"($io_print("hello runner");)";
        _h.gui->send(proto::make_exec(req));
        const outcome o = drain(_h, 10000);
        check(o.output == "hello runner", "output is byte exact");
        check(o.got_result && str_field(o.result, "errorName") == "NoError", "run reports NoError");
        check(o.got_exited && int_field(o.exited, "code") == 0, "exit code 0");
        check(!o.seqs.empty() && o.seqs.front() == 1, "output seq starts at 1");
    }

    // test_bulk — 1e6 lines through the lossless queue
    void test_bulk(harness& _h) {
        std::printf("bulk: 1e6 lines\n");
        proto::exec_request req;
        req.mode = "text";
        req.text = "var i = 0;\nwhile (i < 1000000) { $io_print(\"line \" + string(i) + \"\\n\"); i++; }\n";
        _h.gui->send(proto::make_exec(req));
        const outcome o = drain(_h, 120000);

        std::string expected;
        expected.reserve(1000000ull * 10);
        for (int i = 0; i < 1000000; ++i) {
            expected += "line ";
            expected += std::to_string(i);
            expected += "\n";
        }
        check(o.output.size() == expected.size(), "output byte count matches");
        check(o.output == expected, "output bytes match");
        check(o.got_result && str_field(o.result, "errorName") == "NoError", "bulk run completes");
        check(o.got_exited && int_field(o.exited, "code") == 0, "bulk exit code 0");

        bool ordered = !o.seqs.empty() && o.seqs.front() == 1;
        for (size_t i = 1; ordered && i < o.seqs.size(); ++i) ordered = o.seqs[i] == o.seqs[i - 1] + 1;
        check(ordered, "output seq strictly ordered");
        check(o.got_result && static_cast<alx::uint_64>(int_field(o.result, "lastOutputSeq")) == o.seqs.size(),
              "lastOutputSeq covers every chunk");
    }

    // test_stop — interrupt an empty infinite loop and check the latency
    void test_stop(harness& _h) {
        std::printf("stop: infinite loop\n");
        proto::exec_request req;
        req.mode = "text";
        req.text = "while (true) { }";
        _h.gui->send(proto::make_exec(req));
        const auto t0 = std::chrono::steady_clock::now();
        const outcome o = drain(_h, 5000, 300);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        check(o.got_result && str_field(o.result, "errorName") == "InterruptedError", "stop reports InterruptedError");
        check(ms < 2500, "stop lands within the 2 s budget");
        check(o.got_exited, "exited arrives after stop");
    }

    // test_compile_error — a syntax error becomes compileError, then ParseError
    void test_compile_error(harness& _h) {
        std::printf("compile: syntax error\n");
        proto::exec_request req;
        req.mode = "text";
        req.text = "var x = ;";
        _h.gui->send(proto::make_exec(req));
        const outcome o = drain(_h, 10000);
        check(o.compile_errors > 0, "at least one compileError");
        check(o.got_result && str_field(o.result, "errorName") == "ParseError", "run reports ParseError");
        check(o.got_exited && int_field(o.exited, "code") == int_field(o.result, "error"), "exit code follows the error");
    }

    // test_input — pipe_in round trip
    void test_input(harness& _h) {
        std::printf("input: one line\n");
        proto::exec_request req;
        req.mode = "text";
        req.text = "var s = $io_input();\n$io_print(\"got \" + s);";
        _h.gui->send(proto::make_exec(req));
        const outcome o = drain(_h, 10000, -1, "alpha");
        check(o.output == "got alpha", "input reaches the script");
        check(o.got_result && str_field(o.result, "errorName") == "NoError", "input run completes");
    }

    // big_text — eight 4 MiB prints; well past the writer's 8 MiB plus comm's 128-pack queue
    std::string big_text() {
        std::string src = "var s = \"x\";\nvar i = 0;\nwhile (i < 22) { s = s + s; i++; }\n";
        for (int i = 0; i < 8; ++i) src += "$io_print(s);\n";
        return src;
    }

    // test_backpressure — a reader that starts late still gets every byte
    void test_backpressure(harness& _h) {
        std::printf("backpressure: slow reader\n");
        proto::exec_request req;
        req.mode = "text";
        req.text = big_text();
        _h.gui->send(proto::make_exec(req));
        _h.gui->pause(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        _h.gui->pause(false);
        const outcome o = drain(_h, 120000);
        const size_t expected = 8u * 4u * 1024u * 1024u;
        check(o.output.size() == expected, "no byte is lost while the reader is slow");
        check(o.output == std::string(expected, 'x'), "slow-reader bytes are exact");
        check(o.got_result && str_field(o.result, "errorName") == "NoError", "slow-reader run completes");
    }

    // test_stop_backpressure — stop must land while output is blocked on backpressure
    void test_stop_backpressure(harness& _h) {
        std::printf("backpressure: stop while blocked\n");
        proto::exec_request req;
        req.mode = "text";
        req.text = big_text();
        _h.gui->send(proto::make_exec(req));
        _h.gui->pause(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        const auto t0 = std::chrono::steady_clock::now();
        _h.gui->send(proto::make_stop(true));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        _h.gui->pause(false);
        const outcome o = drain(_h, 15000);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        check(o.got_result && str_field(o.result, "errorName") == "InterruptedError",
              "stop breaks a writer blocked on backpressure");
        check(ms < 2500, "blocked stop lands within the 2 s budget");
        check(o.got_exited, "exited arrives after blocked stop");
    }


    // frame_field — one field of a stopped frame
    alx::int_64 frame_int(const alx::variant& _f, const char* _key) {
        if (!_f.is<alx::varmap>()) return 0;
        const alx::varmap& m = _f.to<alx::varmap>();
        if (!m.contain(_key)) return 0;
        return m.value(_key).to<alx::int_64>(0);
    }

    std::string frame_str(const alx::variant& _f, const char* _key) {
        if (!_f.is<alx::varmap>()) return std::string();
        const alx::varmap& m = _f.to<alx::varmap>();
        if (!m.contain(_key)) return std::string();
        return m.value(_key).to<std::string>();
    }

    // dbg — drives a paused session: collects output and answers snapshots
    struct dbg {
        harness& h;
        outcome o;
        proto::message stopped;

        bool pump(proto::message& _m, int _ms) {
            if (!h.gui->pop(_m, _ms)) return false;
            if (_m.type == proto::kOutput) {
                if (_m.payload.contain("data")) {
                    const alx::bytes& b = _m.payload.value("data").to<alx::bytes>();
                    if (b.size()) o.output.append(reinterpret_cast<const char*>(b.data()), b.size());
                }
                o.seqs.push_back(static_cast<alx::uint_64>(int_field(_m, "seq")));
                return false;
            }
            if (_m.type == proto::kLog) {
                std::printf("  runner log [%s] %s\n", str_field(_m, "level").c_str(), str_field(_m, "text").c_str());
                return false;
            }
            return true;
        }

        bool wait_stopped(int _ms) {
            const auto t0 = std::chrono::steady_clock::now();
            while (std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0)
                       .count() < _ms) {
                proto::message m;
                if (!pump(m, 20)) continue;
                if (m.type == proto::kStopped) {
                    stopped = m;
                    return true;
                }
                if (m.type == proto::kResult) o.result = m;
                if (m.type == proto::kExited) o.exited = m;
            }
            return false;
        }

        bool wait_snapshot(proto::message& _out, int _ms) {
            const auto t0 = std::chrono::steady_clock::now();
            while (std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0)
                       .count() < _ms) {
                proto::message m;
                if (!pump(m, 20)) continue;
                if (m.type == proto::kSnapshot) {
                    _out = m;
                    return true;
                }
                if (m.type == proto::kResult) o.result = m;
                if (m.type == proto::kExited) o.exited = m;
            }
            return false;
        }

        bool wait_view(proto::message& _out, int _ms) {
            const auto t0 = std::chrono::steady_clock::now();
            while (std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0)
                       .count() < _ms) {
                proto::message m;
                if (!pump(m, 20)) continue;
                if (m.type == proto::kView) {
                    _out = m;
                    return true;
                }
                if (m.type == proto::kResult) o.result = m;
                if (m.type == proto::kExited) o.exited = m;
            }
            return false;
        }

        bool wait_done(int _ms) {
            const auto t0 = std::chrono::steady_clock::now();
            while (std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0)
                       .count() < _ms) {
                proto::message m;
                if (!pump(m, 20)) continue;
                if (m.type == proto::kResult) o.result = m;
                if (m.type == proto::kExited) o.exited = m;
                if (o.result.type == proto::kResult && o.exited.type == proto::kExited) {
                    h.gui->send(proto::make_shutdown()); // the GUI's ack lets the runner exit
                    return true;
                }
            }
            return false;
        }
    };

    // node_field — one field of a snapshot node
    std::string node_str(const alx::variant& _n, const char* _key) {
        if (!_n.is<alx::varmap>()) return std::string();
        const alx::varmap& m = _n.to<alx::varmap>();
        if (!m.contain(_key)) return std::string();
        return m.value(_key).to<std::string>();
    }

    // find_node — node by name in a snapshot reply
    const alx::variant* find_node(const proto::message& _m, const std::string& _name) {
        if (!_m.payload.contain("nodes")) return nullptr;
        for (const auto& n : _m.payload.value("nodes").to<alx::varvec>())
            if (node_str(n, "name") == _name) return &n;
        return nullptr;
    }

    // debug_entry — stopOnEntry freezes before the first statement
    void test_debug_entry(harness& _h) {
        std::printf("debug: stop on entry\n");
        dbg d{_h};
        proto::exec_request req;
        req.mode = "text";
        req.source_id = "buf:entry";
        req.text = "var a = 1;\nvar b = 2;\n$io_print(string(a + b));";
        req.debug = true;
        req.stop_on_entry = true;
        _h.gui->send(proto::make_exec(req));

        check(d.wait_stopped(5000), "entry stop arrives");
        check(str_field(d.stopped, "reason") == "entry", "reason is entry");
        const alx::varvec& frames = d.stopped.payload.value("frames").to<alx::varvec>();
        check(!frames.empty(), "frames are reported");
        check(!frames.empty() && frame_int(frames.front(), "line") == 1, "entry line is 1");
        check(!frames.empty() && frame_str(frames.front(), "source") == "buf:entry", "entry source id");

        _h.gui->send(proto::make_continue());
        check(d.wait_done(10000), "run finishes after continue");
        check(str_field(d.o.result, "errorName") == "NoError", "entry run is ok");
        check(d.o.output == "3", "entry run output");
    }

    // debug_breakpoint — a line breakpoint freezes with the locals in reach
    void test_debug_breakpoint(harness& _h) {
        std::printf("debug: line breakpoint\n");
        dbg d{_h};
        proto::exec_request req;
        req.mode = "text";
        req.source_id = "buf:bp";
        req.text = "var a = 1;\nvar b = 2;\nvar c = a + b;\n$io_print(string(c));";
        req.debug = true;
        req.breakpoints.push_back({"buf:bp", 3, true});
        _h.gui->send(proto::make_exec(req));

        check(d.wait_stopped(5000), "breakpoint stop arrives");
        check(str_field(d.stopped, "reason") == "breakpoint", "reason is breakpoint");
        const alx::varvec& frames = d.stopped.payload.value("frames").to<alx::varvec>();
        check(!frames.empty() && frame_int(frames.front(), "line") == 3, "stopped on line 3");

        _h.gui->send(proto::make_snapshot_request("root"));
        proto::message snap;
        check(d.wait_snapshot(snap, 5000), "root snapshot arrives");
        const alx::variant* a = find_node(snap, "a");
        const alx::variant* b = find_node(snap, "b");
        check(a && node_str(*a, "value") == "1", "local a is 1");
        check(b && node_str(*b, "value") == "2", "local b is 2");
        check(!find_node(snap, "c"), "c is not assigned yet");

        _h.gui->send(proto::make_continue());
        check(d.wait_done(10000), "run finishes after continue");
        check(d.o.output == "3", "breakpoint run output");
    }

    // debug_view — a cv handle answers desc/BMP; a missing ref and a non-handle fail loudly
    void test_debug_view(harness& _h) {
        std::printf("debug: view\n");
        if (!std::filesystem::exists("bin/links/cv.so")) {
            std::printf("  skipped: bin/links/cv.so is not built\n");
            return;
        }
        dbg d{_h};
        proto::exec_request req;
        req.mode = "text";
        req.source_id = "buf:view";
        req.text =
            "link \"bin/links/cv\" as cv;\n"
            "var m = cv.create(32, 16, \"8uc3\", 0x00FF00);\n"
            "var t = $thd_create(\"var x = 1;\");\n"
            "var x = 7;\n"
            "$io_print(\"ok\");";
        req.debug = true;
        req.breakpoints.push_back({"buf:view", 5, true});
        _h.gui->send(proto::make_exec(req));
        check(d.wait_stopped(5000), "view stop arrives");

        _h.gui->send(proto::make_view_request("root/m", 0, 1));
        proto::message view;
        check(d.wait_view(view, 5000), "view answer arrives");
        check(view.id == 1, "view answer echoes the request id");
        check(str_field(view, "ref") == "root/m", "view ref echoes");
        check(bool_field(view, "ok"), "view ok");
        if (view.payload.contain("view")) {
            const alx::varmap& vp = view.payload.value("view").to<alx::varmap>();
            check(vp.value("kind").to<std::string>() == "image", "view kind is image");
            check(vp.value("format").to<std::string>() == "bmp", "view format is bmp");
            check(vp.value("width").to<alx::int_64>() == 32 &&
                      vp.value("height").to<alx::int_64>() == 16,
                  "view dimensions");
            const alx::bytes& bmp = vp.value("data").to<alx::bytes>();
            check(bmp.size() > 2 && reinterpret_cast<const char*>(bmp.data())[0] == 'B' &&
                      reinterpret_cast<const char*>(bmp.data())[1] == 'M',
                  "view data is a BMP");
        }

        _h.gui->send(proto::make_snapshot_request("root"));
        proto::message snap;
        check(d.wait_snapshot(snap, 5000), "root snapshot arrives with the thread");
        const alx::variant* t = find_node(snap, "t");
        check(t && node_str(*t, "type") == "thread", "thread handle is named by the host table");
        check(t && (node_str(*t, "value") == "running" || node_str(*t, "value") == "finished"),
              "thread handle carries a host summary");

        _h.gui->send(proto::make_view_request("root/t", 0, 4));
        proto::message tview;
        check(d.wait_view(tview, 5000), "thread view answer arrives");
        check(bool_field(tview, "ok"), "thread view ok");
        if (tview.payload.contain("view")) {
            const alx::varmap& tvp = tview.payload.value("view").to<alx::varmap>();
            check(tvp.value("kind").to<std::string>() == "text", "thread view kind is text");
            const alx::bytes& tb = tvp.value("data").to<alx::bytes>();
            const std::string text(reinterpret_cast<const char*>(tb.data()), tb.size());
            check(text.find("state:") != std::string::npos, "thread view carries its state");
        }

        _h.gui->send(proto::make_view_request("root/nope", 0, 2));
        proto::message missing;
        check(d.wait_view(missing, 5000), "missing-ref answer arrives");
        check(!bool_field(missing, "ok"), "missing ref fails");
        check(str_field(missing, "error").find("does not resolve") != std::string::npos,
              "missing ref names the failure");

        _h.gui->send(proto::make_view_request("root/x", 0, 3));
        proto::message nonhandle;
        check(d.wait_view(nonhandle, 5000), "non-handle answer arrives");
        check(!bool_field(nonhandle, "ok"), "non-handle fails");
        check(str_field(nonhandle, "error").find("requires a handle") != std::string::npos,
              "non-handle names the failure");

        _h.gui->send(proto::make_continue());
        check(d.wait_done(10000), "view run finishes");
        check(d.o.output == "ok", "view run output");
    }

    // debug_step — step over moves one statement
    void test_debug_step(harness& _h) {
        std::printf("debug: step over\n");
        dbg d{_h};
        proto::exec_request req;
        req.mode = "text";
        req.source_id = "buf:step";
        req.text = "var a = 1;\nvar b = 2;\n$io_print(string(a + b));";
        req.debug = true;
        req.stop_on_entry = true;
        _h.gui->send(proto::make_exec(req));
        check(d.wait_stopped(5000), "entry stop arrives");

        _h.gui->send(proto::make_step("over"));
        check(d.wait_stopped(5000), "step stop arrives");
        check(str_field(d.stopped, "reason") == "step", "reason is step");
        const alx::varvec& frames = d.stopped.payload.value("frames").to<alx::varvec>();
        check(!frames.empty() && frame_int(frames.front(), "line") == 2, "stepped to line 2");

        _h.gui->send(proto::make_continue());
        check(d.wait_done(10000), "run finishes after continue");
        check(d.o.output == "3", "step run output");
    }

    // debug_frames — a breakpoint inside a def exposes that frame's locals
    void test_debug_frames(harness& _h) {
        std::printf("debug: frame locals\n");
        dbg d{_h};
        proto::exec_request req;
        req.mode = "text";
        req.source_id = "buf:fn";
        req.text = "def add(a, b) {\n    var s = a + b;\n    $io_print(string(s));\n    return s;\n}\n"
                   "var r = add(2, 3);";
        req.debug = true;
        req.breakpoints.push_back({"buf:fn", 3, true});
        _h.gui->send(proto::make_exec(req));

        check(d.wait_stopped(5000), "function stop arrives");
        const alx::varvec& frames = d.stopped.payload.value("frames").to<alx::varvec>();
        check(frames.size() == 2, "two frames: add and root");
        check(frames.size() == 2 && frame_str(frames.front(), "func") == "add", "inner frame is add");

        _h.gui->send(proto::make_snapshot_request("f0"));
        proto::message snap;
        check(d.wait_snapshot(snap, 5000), "frame snapshot arrives");
        const alx::variant* a = find_node(snap, "a");
        const alx::variant* b = find_node(snap, "b");
        const alx::variant* s = find_node(snap, "s");
        check(a && node_str(*a, "value") == "2", "arg a is 2");
        check(b && node_str(*b, "value") == "3", "arg b is 3");
        check(s && node_str(*s, "value") == "5", "local s is 5");

        _h.gui->send(proto::make_continue());
        check(d.wait_done(10000), "run finishes after continue");
        check(d.o.output == "5", "function run output");
    }

    // debug_nested_frame — the front frame's ref names that frame, not the outermost one
    void test_debug_nested_frame(harness& _h) {
        std::printf("debug: nested frame ref\n");
        dbg d{_h};
        proto::exec_request req;
        req.mode = "text";
        req.source_id = "buf:nest";
        req.text = "def inner(x) {\n    var y = x + 1;\n    $io_print(string(y));\n    return y;\n}\n"
                   "def outer(n) {\n    return inner(n);\n}\nvar r = outer(1);";
        req.debug = true;
        req.breakpoints.push_back({"buf:nest", 3, true});
        _h.gui->send(proto::make_exec(req));

        check(d.wait_stopped(5000), "nested stop arrives");
        const alx::varvec& frames = d.stopped.payload.value("frames").to<alx::varvec>();
        check(frames.size() == 3, "three frames: inner, outer, root");
        check(frames.size() == 3 && frame_str(frames.front(), "func") == "inner", "front frame is inner");
        const std::string ref = frames.empty() ? std::string() : frame_str(frames.front(), "ref");
        check(ref == "f1", "inner frame carries its engine index");

        _h.gui->send(proto::make_snapshot_request(ref));
        proto::message snap;
        check(d.wait_snapshot(snap, 5000), "front-frame snapshot arrives");
        const alx::variant* x = find_node(snap, "x");
        const alx::variant* y = find_node(snap, "y");
        check(x && node_str(*x, "value") == "1", "inner arg x is 1");
        check(y && node_str(*y, "value") == "2", "inner local y is 2");

        _h.gui->send(proto::make_snapshot_request("f0"));
        proto::message outer;
        check(d.wait_snapshot(outer, 5000), "outer frame snapshot arrives");
        check(find_node(outer, "n") != nullptr, "outer frame has its own parameter");
        check(find_node(outer, "y") == nullptr, "inner local is not in the outer frame");

        _h.gui->send(proto::make_continue());
        check(d.wait_done(10000), "nested run finishes");
        check(d.o.output == "2", "nested run output");
    }

    // debug_snapshot_page — offset pages cover every local exactly once
    void test_debug_snapshot_page(harness& _h) {
        std::printf("debug: snapshot pages\n");
        dbg d{_h};
        proto::exec_request req;
        req.mode = "text";
        req.source_id = "buf:page";
        req.text = "var a = 1;\nvar b = 2;\nvar c = 3;\nvar d = 4;\n$io_print(\"ok\");";
        req.debug = true;
        req.breakpoints.push_back({"buf:page", 5, true});
        _h.gui->send(proto::make_exec(req));
        check(d.wait_stopped(5000), "page stop arrives");

        std::vector<std::string> names;
        for (alx::uint_64 ofst = 0; ofst < 4; ofst += 2) {
            _h.gui->send(proto::make_snapshot_request("root", 2, ofst));
            proto::message snap;
            check(d.wait_snapshot(snap, 5000), "page arrives");
            check(static_cast<alx::uint_64>(int_field(snap, "ofst")) == ofst, "page offset echoes");
            const bool more = snap.payload.contain("more") && snap.payload.value("more").to<bool>(false);
            check(more == (ofst == 0), ofst == 0 ? "first page reports more" : "last page reports no more");
            if (snap.payload.contain("nodes"))
                for (const auto& nv : snap.payload.value("nodes").to<alx::varvec>())
                    names.push_back(node_str(nv, "name"));
        }
        std::sort(names.begin(), names.end());
        check(names.size() == 4 && names[0] == "a" && names[1] == "b" && names[2] == "c" &&
                  names[3] == "d",
              "both pages cover every local exactly once");

        _h.gui->send(proto::make_continue());
        check(d.wait_done(10000), "page run finishes");
        check(d.o.output == "ok", "page run output");
    }

    // debug_stepio — step into a def, then step back out to the caller
    void test_debug_stepio(harness& _h) {
        std::printf("debug: step into and out\n");
        dbg d{_h};
        proto::exec_request req;
        req.mode = "text";
        req.source_id = "buf:sio";
        req.text = "def add(a, b) {\n    return a + b;\n}\nvar r = add(1, 2);\n$io_print(string(r));";
        req.debug = true;
        req.breakpoints.push_back({"buf:sio", 4, true});
        _h.gui->send(proto::make_exec(req));
        check(d.wait_stopped(5000), "call-line breakpoint stops");

        _h.gui->send(proto::make_step("in"));
        check(d.wait_stopped(5000), "step-into stop arrives");
        const alx::varvec& in = d.stopped.payload.value("frames").to<alx::varvec>();
        check(in.size() == 2, "step into has two frames");
        check(in.size() == 2 && frame_str(in.front(), "func") == "add", "stepped into add");
        check(in.size() == 2 && frame_int(in.front(), "line") == 2, "inside add at line 2");

        _h.gui->send(proto::make_step("out"));
        check(d.wait_stopped(5000), "step-out stop arrives");
        const alx::varvec& out = d.stopped.payload.value("frames").to<alx::varvec>();
        check(out.size() == 1, "step out returns to the caller frame");
        check(out.size() == 1 && frame_int(out.front(), "line") == 5, "back in the caller after the call");

        _h.gui->send(proto::make_continue());
        check(d.wait_done(10000), "step-io run finishes");
        check(d.o.output == "3", "step-io run output");
    }

    // wait_result — console runs send a result per exec and no exited
    bool wait_result(dbg& _d, int _ms) {
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0)
                   .count() < _ms) {
            proto::message m;
            if (!_d.pump(m, 20)) continue;
            if (m.type == proto::kResult) {
                _d.o.result = m;
                return true;
            }
            if (m.type == proto::kExited) _d.o.exited = m;
        }
        return false;
    }

    // console — one process runs several execs; reset drops the state
    void test_console(harness& _h) {
        std::printf("console: repeated exec\n");
        dbg d{_h};
        proto::exec_request req;
        req.mode = "text";
        req.source_id = "buf:con";
        req.console = true;

        req.text = "var x = 1;";
        _h.gui->send(proto::make_exec(req));
        check(wait_result(d, 5000), "first console result");
        check(str_field(d.o.result, "errorName") == "NoError", "first exec is ok");

        req.text = "x = x + 1;\n$io_print(string(x));";
        _h.gui->send(proto::make_exec(req));
        check(wait_result(d, 5000), "second console result");
        check(d.o.output == "2", "console keeps the script state");

        _h.gui->send(proto::make_reset());
        req.text = "$io_print(string(x));";
        _h.gui->send(proto::make_exec(req));
        check(wait_result(d, 5000), "third console result");
        check(str_field(d.o.result, "errorName") != "NoError", "reset dropped the root state");

        _h.gui->send(proto::make_shutdown());
        check(_h.ctrl.wait(3000), "console process exits on shutdown");
    }

    // bench — informational: runner round trip for a compute-only script
    void test_bench(harness& _h) {
        std::printf("bench: 10M iterations\n");
        proto::exec_request req;
        req.mode = "text";
        req.text = "var i = 0;\nwhile (i < 10000000) { i++; }";
        const auto t0 = std::chrono::steady_clock::now();
        _h.gui->send(proto::make_exec(req));
        const outcome o = drain(_h, 60000);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        std::printf("  bench: runner %lld ms, engine %lld ms (error=%s)\n", static_cast<long long>(ms),
                    static_cast<long long>(int_field(o.result, "elapsedUs") / 1000),
                    str_field(o.result, "errorName").c_str());
        check(o.got_result && str_field(o.result, "errorName") == "NoError", "bench run completes");
    }

    // debug_logpoint — a breakpoint with a message logs and keeps running
    void test_debug_logpoint(harness& _h) {
        std::printf("debug: logpoint\n");
        dbg d{_h};
        proto::exec_request req;
        req.mode = "text";
        req.source_id = "buf:log";
        req.text = "var a = 1;\nvar b = 2;\n$io_print(string(a + b));";
        req.debug = true;
        req.breakpoints.push_back({"buf:log", 3, true, "logpoint line 3"});
        _h.gui->send(proto::make_exec(req));
        check(d.wait_done(10000), "logpoint run finishes without pausing");
        check(str_field(d.o.result, "errorName") == "NoError", "logpoint run is ok");
        check(d.o.output.find("logpoint line 3") != std::string::npos, "logpoint text is printed");
        check(d.o.output.find("3") != std::string::npos, "the script itself still runs");
    }

    // debug_condition — a conditional breakpoint stops only when the predicate holds
    void test_debug_condition(harness& _h) {
        std::printf("debug: conditional breakpoint\n");
        dbg d{_h};
        proto::exec_request req;
        req.mode = "text";
        req.source_id = "buf:cond";
        req.text = "var i = 0;\nwhile (i < 10) {\n    i++;\n}\n$io_print(string(i));";
        req.debug = true;
        req.breakpoints.push_back({"buf:cond", 3, true, "", "i == 5"});
        _h.gui->send(proto::make_exec(req));

        check(d.wait_stopped(10000), "conditional stop arrives");
        const alx::varvec& frames = d.stopped.payload.value("frames").to<alx::varvec>();
        check(!frames.empty() && frame_int(frames.front(), "line") == 3, "stopped on the loop line");

        _h.gui->send(proto::make_snapshot_request("root"));
        proto::message snap;
        check(d.wait_snapshot(snap, 5000), "condition snapshot arrives");
        const alx::variant* i = find_node(snap, "i");
        check(i && node_str(*i, "value") == "5", "i is 5 at the stop");

        _h.gui->send(proto::make_continue());
        check(d.wait_done(10000), "conditional run finishes");
        check(d.o.output == "10", "loop ran to completion");
    }

    // debug_setbreak — breakpoints can be replaced while the run is paused
    void test_debug_setbreak(harness& _h) {
        std::printf("debug: set breakpoints while paused\n");
        dbg d{_h};
        proto::exec_request req;
        req.mode = "text";
        req.source_id = "buf:set";
        req.text = "var a = 1;\nvar b = 2;\n$io_print(string(a + b));";
        req.debug = true;
        req.stop_on_entry = true;
        _h.gui->send(proto::make_exec(req));
        check(d.wait_stopped(5000), "entry stop arrives");

        _h.gui->send(proto::make_set_breakpoints("buf:set", {{"buf:set", 2, true}}));
        _h.gui->send(proto::make_continue());
        check(d.wait_stopped(5000), "new breakpoint stops the run");
        check(str_field(d.stopped, "reason") == "breakpoint", "reason is breakpoint");
        const alx::varvec& frames = d.stopped.payload.value("frames").to<alx::varvec>();
        check(!frames.empty() && frame_int(frames.front(), "line") == 2, "stopped on line 2");

        _h.gui->send(proto::make_continue());
        check(d.wait_done(10000), "run finishes after continue");
        check(d.o.output == "3", "set-breakpoint run output");
    }

    // debug_setbreak_live — a breakpoint added mid-run takes effect without a pause
    void test_debug_setbreak_live(harness& _h) {
        std::printf("debug: set breakpoints while running\n");
        dbg d{_h};
        proto::exec_request req;
        req.mode = "text";
        req.source_id = "buf:live";
        req.text = "var i = 0;\nwhile (i < 5000000) { i = i + 1; }\n$io_print(string(i));";
        req.debug = true;
        _h.gui->send(proto::make_exec(req));
        _h.gui->send(proto::make_set_breakpoints("buf:live", {{"buf:live", 3, true}}));
        check(d.wait_stopped(15000), "mid-run breakpoint stops the run");
        check(str_field(d.stopped, "reason") == "breakpoint", "reason is breakpoint");
        const alx::varvec& frames = d.stopped.payload.value("frames").to<alx::varvec>();
        check(!frames.empty() && frame_int(frames.front(), "line") == 3, "stopped on line 3");

        _h.gui->send(proto::make_continue());
        check(d.wait_done(15000), "run finishes after continue");
        check(d.o.output == "5000000", "loop ran to completion");
    }

    // debug_stop — stop while paused ends the run as interrupted
    // debug_ax — a .ax project run stops inside its main script
    void test_debug_ax(harness& _h) {
        std::printf("debug: .ax project\n");
        const std::string dir = "/tmp/axc-replay-ax";
        std::system(("mkdir -p " + dir).c_str());
        {
            std::ofstream main(dir + "/main.axc", std::ios::trunc);
            main << "var a = 1;\nvar b = 2;\n$io_print(string(a + b));\n";
        }
        {
            std::ofstream proj(dir + "/proj.ax", std::ios::trunc);
            proj << "{\"main\": \"main.axc\"}";
        }
        dbg d{_h};
        proto::exec_request req;
        req.mode = "ax";
        req.path = dir + "/proj.ax";
        req.debug = true;
        req.stop_on_entry = true;
        _h.gui->send(proto::make_exec(req));

        check(d.wait_stopped(5000), "project entry stop arrives");
        const alx::varvec& frames = d.stopped.payload.value("frames").to<alx::varvec>();
        check(!frames.empty() && frame_int(frames.front(), "line") == 1, "project stop is line 1");
        check(!frames.empty() && frame_str(frames.front(), "source").find("main.axc") != std::string::npos,
              "project source is its main script");

        _h.gui->send(proto::make_continue());
        check(d.wait_done(10000), "project run finishes");
        check(d.o.output == "3", "project output");
    }

    void test_debug_stop(harness& _h) {
        std::printf("debug: stop while paused\n");
        dbg d{_h};
        proto::exec_request req;
        req.mode = "text";
        req.source_id = "buf:stop";
        req.text = "var a = 1;\nvar b = 2;";
        req.debug = true;
        req.stop_on_entry = true;
        _h.gui->send(proto::make_exec(req));
        check(d.wait_stopped(5000), "entry stop arrives");

        const auto t0 = std::chrono::steady_clock::now();
        _h.gui->send(proto::make_stop(false));
        check(d.wait_done(5000), "stop ends the paused run");
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        check(str_field(d.o.result, "errorName") == "InterruptedError", "paused stop is interrupted");
        check(ms < 2500, "paused stop lands within 2 s");
    }

    // test_crash — SIGKILL the runner and prove the GUI half survives, classified
    void test_crash(harness& _h) {
#ifndef _WIN32
        std::printf("crash: SIGKILL mid-run\n");
        proto::exec_request req;
        req.mode = "text";
        req.text = "while (true) { }";
        _h.gui->send(proto::make_exec(req));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        const alx::uint_32 child = _h.ctrl.pid();
        check(child != 0, "runner has a pid");
        ::kill(static_cast<int>(child), SIGKILL);

        check(_h.ctrl.wait(3000), "dead runner is noticed");
        const alx::process_ctrl::wait_status st = _h.ctrl.status();
        check(st.exited && st.signaled && st.term_sig == SIGKILL, "death classified as SIGKILL");
        _h.ctrl.kill();
#endif
    }

    // test_frame_size — T=0: frames close at the size cap, byte-exact, seq contiguous
    void test_frame_size(harness& _h) {
        std::printf("frame: size cap\n");
        proto::exec_request req;
        req.mode = "text";
        req.text = "var s = \"0123456789\";\nvar i = 0;\nwhile (i < 100000) { $io_print(s); i++; }\n";
        _h.gui->send(proto::make_exec(req));
        const outcome o = drain(_h, 60000);
        check(o.output.size() == 1000000, "frame output is byte exact");
        size_t max_packet = 0;
        for (size_t n : o.packets) max_packet = std::max(max_packet, n);
        check(!o.packets.empty() && max_packet <= 32u * 1024u, "no packet exceeds the frame cap");
        check(o.packets.size() >= 30, "frames are smaller than the legacy 64 KiB chunks");
        bool ordered = !o.seqs.empty() && o.seqs.front() == 1;
        for (size_t i = 1; ordered && i < o.seqs.size(); ++i) ordered = o.seqs[i] == o.seqs[i - 1] + 1;
        check(ordered, "frame seq is contiguous");
        check(o.got_result && static_cast<alx::uint_64>(int_field(o.result, "lastOutputSeq")) == o.seqs.size(),
              "lastOutputSeq covers every frame");
    }

    // test_frame_event — a long deadline must not hold output back when the run ends
    void test_frame_event(harness& _h) {
        std::printf("frame: event flush\n");
        proto::exec_request req;
        req.mode = "text";
        req.text = "$io_print(\"hello\");";
        const auto t0 = std::chrono::steady_clock::now();
        _h.gui->send(proto::make_exec(req));
        const outcome o = drain(_h, 10000);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        check(o.output == "hello", "pending frame is flushed before result");
        check(o.got_result && o.got_exited, "event-flushed run completes");
        check(ms < 300, "event flush does not wait for the deadline");
    }

    // test_trim — one oversized write is cut to K plus a marker
    void test_trim(harness& _h) {
        std::printf("trim: oversized write\n");
        proto::exec_request req;
        req.mode = "text";
        req.text = "var s = \"x\";\nvar i = 0;\nwhile (i < 22) { s = s + s; i++; }\n$io_print(s);\n";
        _h.gui->send(proto::make_exec(req));
        const outcome o = drain(_h, 30000);
        check(o.output.size() < 100u * 1024u, "trimmed display is bounded");
        check(o.output.find("已省略") != std::string::npos, "the trim marker is present");
        check(o.output.find("完整内容未保存") != std::string::npos, "marker names the missing log");
        check(o.got_result && str_field(o.result, "errorName") == "NoError", "trimmed run completes");
    }

    // drain_without_stops — consume until result+exited and report how many stops arrived
    int drain_without_stops(harness& _h, outcome& _o, bool& _done, int _ms = 10000) {
        dbg d{_h};
        int stops = 0;
        const auto t0 = std::chrono::steady_clock::now();
        while (!_done && std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                                 .count() < _ms) {
            proto::message m;
            if (!d.pump(m, 20)) {
                _done = _o.result.type == proto::kResult && _o.exited.type == proto::kExited;
                continue;
            }
            if (m.type == proto::kStopped) ++stops;
            if (m.type == proto::kResult) _o.result = m;
            if (m.type == proto::kExited) _o.exited = m;
            if (_o.result.type == proto::kResult && _o.exited.type == proto::kExited) {
                _h.gui->send(proto::make_shutdown());
                _done = true;
            }
        }
        _o.output = d.o.output;
        return stops;
    }

    // mt_script — a child that traps while the main waits in thd_join
    const char* mt_script() {
        return "var h = $thd_create(\"var a = 41;\\ntrap(\\\"child here\\\");\\n$io_print(\\\"child\\\");\");\n"
               "$thd_join(h);\n"
               "$io_print(\"done\");";
    }

    // test_mt_trap — a child trap stops the session; its own engine serves the snapshot
    void test_mt_trap(harness& _h) {
        std::printf("mt: child trap\n");
        dbg d{_h};
        proto::exec_request req;
        req.mode = "text";
        req.source_id = "buf:mt";
        req.text = mt_script();
        req.debug = true;
        req.mt_debug = true;
        _h.gui->send(proto::make_exec(req));

        check(d.wait_stopped(10000), "child trap stop arrives");
        const alx::int_64 thread = int_field(d.stopped, "thread");
        check(thread != 0, "the stopping thread is a child");
        check(int_field(d.stopped, "trigger") == thread, "the child is the trigger");
        check(int_field(d.stopped, "parent") == 0, "the child hangs off the main thread");
        check(int_field(d.stopped, "known") == 2, "known counts both engines");
        check(str_field(d.stopped, "reason") == "trap", "reason is trap");
        check(str_field(d.stopped, "label") == "child here", "label carries the trap string");
        check(str_field(d.stopped, "trapArgs").find("child here") != std::string::npos,
              "trapArgs carries the payload");
        const alx::varvec& frames = d.stopped.payload.value("frames").to<alx::varvec>();
        check(!frames.empty(), "child frames are reported");
        check(!frames.empty() && frame_int(frames.front(), "ofst") > 0, "frame carries the byte offset");

        _h.gui->send(proto::make_snapshot_request(
            "root", 0, 0, static_cast<alx::uint_64>(thread)));
        proto::message snap;
        check(d.wait_snapshot(snap, 5000), "child snapshot arrives");
        check(int_field(snap, "thread") == thread, "snapshot echoes the thread");
        check(find_node(snap, "a") != nullptr, "child root locals are visible");

        _h.gui->send(proto::make_continue());
        check(d.wait_done(10000), "run finishes after continue");
        check(d.o.output == "childdone", "child and main output in order");
        check(str_field(d.o.result, "errorName") == "NoError", "mt run is ok");
    }

    // test_mt_sync — a child trap stops the main thread at its next hook point
    void test_mt_sync(harness& _h) {
        std::printf("mt: stop the world\n");
        dbg d{_h};
        proto::exec_request req;
        req.mode = "text";
        req.text =
            "var h = $thd_create(\"trap(\\\"c\\\");\");\n"
            "var i = 0;\n"
            "while (i < 200000) { i++; }\n"
            "$thd_join(h);\n"
            "$io_print(\"done\");";
        req.debug = true;
        req.mt_debug = true;
        _h.gui->send(proto::make_exec(req));

        proto::message got[2];
        int count = 0;
        const auto t0 = std::chrono::steady_clock::now();
        while (count < 2 && std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                    .count() < 10000) {
            proto::message m;
            if (!d.pump(m, 20)) continue;
            if (m.type == proto::kStopped) got[count++] = m;
        }
        check(count == 2, "both threads report");
        if (count == 2) {
            const proto::message* child = int_field(got[0], "thread") != 0 ? &got[0] : &got[1];
            const proto::message* main = child == &got[0] ? &got[1] : &got[0];
            const alx::int_64 child_id = int_field(*child, "thread");
            check(child_id != 0, "one of them is a child");
            check(int_field(*child, "trigger") == child_id, "the child is the trigger");
            check(str_field(*child, "reason") == "trap", "the child stopped on its trap");
            check(int_field(*main, "thread") == 0, "the main thread parks");
            check(str_field(*main, "reason") == "sync", "the main arrival is a sync stop");
            check(int_field(*main, "stop") == int_field(*child, "stop"), "one stop wave");
            check(int_field(*main, "known") == 2, "both engines are known");
        }

        _h.gui->send(proto::make_continue());
        check(d.wait_done(10000), "run finishes after continue");
        check(d.o.output == "done", "output after the wave");
    }

    // test_mt_off — the switch is off: child traps stay silent and nothing stops
    void test_mt_off(harness& _h) {
        std::printf("mt: switch off\n");
        proto::exec_request req;
        req.mode = "text";
        req.text = mt_script();
        req.debug = true;
        req.mt_debug = false;
        _h.gui->send(proto::make_exec(req));

        outcome o;
        bool done = false;
        const int stops = drain_without_stops(_h, o, done);
        check(stops == 0, "no stop without multi-thread debugging");
        check(done, "run completes");
        check(o.output == "childdone", "both threads still run");
        check(str_field(o.result, "errorName") == "NoError", "off run is ok");
    }

    // test_mt_skip — 跳过Trap also applies to child traps
    void test_mt_skip(harness& _h) {
        std::printf("mt: skipped child traps\n");
        proto::exec_request req;
        req.mode = "text";
        req.text = mt_script();
        req.debug = true;
        req.mt_debug = true;
        req.skip_traps = true;
        _h.gui->send(proto::make_exec(req));

        outcome o;
        bool done = false;
        const int stops = drain_without_stops(_h, o, done);
        check(stops == 0, "a skipped child trap does not stop");
        check(done, "run completes");
        check(o.output == "childdone", "both threads still run");
        check(str_field(o.result, "errorName") == "NoError", "skipped-trap run is ok");
    }

    // test_mt_pause — pause reaches a child loop while the main sits in thd_join
    void test_mt_pause(harness& _h) {
        std::printf("mt: pause a child loop\n");
        dbg d{_h};
        proto::exec_request req;
        req.mode = "text";
        req.text =
            "var h = $thd_create(\"var i = 0;\\nwhile (true) { i++; }\");\n"
            "$thd_join(h);";
        req.debug = true;
        req.mt_debug = true;
        _h.gui->send(proto::make_exec(req));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        _h.gui->send(proto::make_pause());

        check(d.wait_stopped(10000), "the child loop stops");
        check(int_field(d.stopped, "thread") != 0, "the child services the pause");
        check(str_field(d.stopped, "reason") == "pause", "reason is pause");

        _h.gui->send(proto::make_stop(false));
        check(d.wait_done(10000), "stop ends the paused run");
        check(str_field(d.o.result, "errorName") == "InterruptedError", "paused child stop interrupts");
    }

    // test_mt_step — stepping the main thread with the switch on
    void test_mt_step(harness& _h) {
        std::printf("mt: main step\n");
        dbg d{_h};
        proto::exec_request req;
        req.mode = "text";
        req.text = "var a = 1;\nvar b = 2;";
        req.debug = true;
        req.mt_debug = true;
        req.stop_on_entry = true;
        _h.gui->send(proto::make_exec(req));

        check(d.wait_stopped(5000), "entry stop arrives");
        check(int_field(d.stopped, "thread") == 0, "main thread is the trigger");
        check(int_field(d.stopped, "known") == 1, "one engine is known");

        _h.gui->send(proto::make_step("over"));
        check(d.wait_stopped(5000), "step stop arrives");
        const alx::varvec& frames = d.stopped.payload.value("frames").to<alx::varvec>();
        check(!frames.empty() && frame_int(frames.front(), "line") == 2, "stepped to line 2");
        check(int_field(d.stopped, "stop") > 1, "the step opens a new stop wave");

        _h.gui->send(proto::make_continue());
        check(d.wait_done(10000), "run finishes after the step");
        check(str_field(d.o.result, "errorName") == "NoError", "step run is ok");
    }

    // test_debug_positions — debug mode still runs, with checkpoints on
    void test_debug(harness& _h) {
        std::printf("debug: statement positions\n");
        proto::exec_request req;
        req.mode = "text";
        req.text = "var n = 0;\nwhile (n < 10) { n++; }\n$io_print(string(n));";
        req.debug = true;
        _h.gui->send(proto::make_exec(req));
        const outcome o = drain(_h, 10000);
        check(o.output == "10", "debug run output");
        check(o.got_result && str_field(o.result, "errorName") == "NoError", "debug run completes");
    }
} // namespace

int main(int _argc, char** _argv) {
    const std::string runner = _argc > 1 ? _argv[1] : "bin/axc-runner";
    // optional scenario names filter, e.g. axc-replay bin/axc-runner text
    const std::vector<std::string> only(_argv + (_argc > 2 ? 2 : _argc), _argv + _argc);

    // config validation — a bad blob dies before connecting, with a key-naming error
    {
        alx::process_ctrl ctrl(runner);
        const bool started = ctrl.start({"--config", "zz"});
        check(started && ctrl.wait(5000) && ctrl.status().exit_code == 2, "bad config exits with code 2");
        ctrl.kill();
    }
    {
        alx::varmap cfg;
        cfg["transport"] = alx::variant(std::string("local"));
        cfg["endpoint"] = alx::variant(std::string("127.0.0.1:1"));
        cfg["token"] = alx::variant(std::string("t"));
        cfg["bogus"] = alx::variant(alx::int_64(1));
        alx::process_ctrl ctrl(runner);
        const bool started = ctrl.start({"--config", alx::varsolid::to_bytes(cfg).to_hex()});
        check(started && ctrl.wait(5000) && ctrl.status().exit_code == 2,
              "unknown config key exits with code 2");
        ctrl.kill();
    }

    struct scenario {
        const char* name;
        void (*fn)(harness&);
        alx::uint_64 frame_ms = 0;
        alx::uint_64 frame_bytes = 0;
        alx::uint_64 trim_bytes = 0;
    };
    const scenario scenarios[] = {
        {"text", test_text},
        {"frame-size", test_frame_size, 0, 32u * 1024u, 0},
        {"frame-event", test_frame_event, 500, 64u * 1024u, 0},
        {"trim", test_trim, 16, 64u * 1024u, 64u * 1024u},
        {"bulk", test_bulk},
        {"stop", test_stop},
        {"compile", test_compile_error},
        {"input", test_input},
        {"backpressure", test_backpressure},
        {"stop-blocked", test_stop_backpressure},
        {"crash", test_crash},
        {"debug", test_debug},
        {"debug-entry", test_debug_entry},
        {"debug-break", test_debug_breakpoint},
        {"debug-view", test_debug_view},
        {"debug-step", test_debug_step},
        {"debug-frames", test_debug_frames},
        {"debug-nested", test_debug_nested_frame},
        {"debug-page", test_debug_snapshot_page},
        {"debug-setbp", test_debug_setbreak},
        {"debug-setbp-live", test_debug_setbreak_live},
        {"debug-ax", test_debug_ax},
        {"debug-stepio", test_debug_stepio},
        {"debug-logpoint", test_debug_logpoint},
        {"debug-condition", test_debug_condition},
        {"console", test_console},
        {"bench", test_bench},
        {"debug-stop", test_debug_stop},
        {"mt-trap", test_mt_trap},
        {"mt-sync", test_mt_sync},
        {"mt-off", test_mt_off},
        {"mt-skip", test_mt_skip},
        {"mt-pause", test_mt_pause},
        {"mt-step", test_mt_step},
    };

    for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); ++i) {
        if (!only.empty() && std::find(only.begin(), only.end(), scenarios[i].name) == only.end()) continue;
        harness h(runner);
        h.frame_ms = scenarios[i].frame_ms;
        h.frame_bytes = scenarios[i].frame_bytes;
        h.trim_bytes = scenarios[i].trim_bytes;
        if (!h.start(static_cast<int>(i))) {
            std::printf("FAIL: harness did not start for %s\n", scenarios[i].name);
            ++g_failures;
            continue;
        }
        scenarios[i].fn(h);
        check(h.ctrl.wait(3000), "runner is not left behind");
    }

    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
