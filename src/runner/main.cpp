// axc-runner — the script execution half of the GUI; no Qt, one run per process.
// argv: --config <hex> — one varsolid blob carrying every option (see usage()).
#include "control.h"
#include "output.h"
#include "session.h"

#include "../proto/config.h"
#include "core/core.h"

#include <acomm_ex.h>
#include <atransmit.h>
#include <avarsolid.h>
#include <avariant.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#    include <process.h>
#else
#    include <unistd.h>
#endif

namespace {
    constexpr const char* kRunnerVersion = "0.1.0";
    constexpr alx::uint_64 kDefaultHighWater = 8u * 1024u * 1024u;
    constexpr int kConnectTries = 100;
    constexpr int kHandshakeMs = 5000;
    constexpr int kLingerMs = 10000;

    std::mutex g_trace_mx;
    std::ofstream g_trace;

    // options — everything the GUI hands over in the config blob
    struct options {
        std::string transport = "local";
        std::string endpoint;
        std::string token;
        std::string trace;
        runner::frame_options frame;
    };

    // config_key — the keys parse_config accepts; a new key must be declared here
    bool config_key(const std::string& _key) {
        using namespace proto::config;
        static const char* kKeys[] = {kTransport, kEndpoint, kToken, kTrace,
                                      kFrameMs, kFrameBytes, kTrimBytes, kLogPath};
        for (const char* k : kKeys)
            if (_key == k) return true;
        return false;
    }

    // take_string — required string key; false with _err set when it is missing or mistyped
    bool take_string(const alx::varmap& _v, const char* _key, std::string& _out, std::string& _err) {
        if (!_v.contain(_key) || !_v.value(_key).is<std::string>()) {
            _err = std::string("config key '") + _key + "' must be a string";
            return false;
        }
        _out = _v.value(_key).to<std::string>();
        return true;
    }

    // take_number — optional integer key; 0 disables, otherwise it must fall in [lo, hi]
    bool take_number(const alx::varmap& _v, const char* _key, alx::uint_64& _out, alx::uint_64 _lo,
                     alx::uint_64 _hi, std::string& _err) {
        if (!_v.contain(_key)) return true;
        if (!_v.value(_key).is<alx::int_64>()) {
            _err = std::string("config key '") + _key + "' must be an integer";
            return false;
        }
        const alx::int_64 n = _v.value(_key).to<alx::int_64>();
        if (n < 0 || (n != 0 && (static_cast<alx::uint_64>(n) < _lo || static_cast<alx::uint_64>(n) > _hi))) {
            _err = std::string("config key '") + _key + "' is out of range";
            return false;
        }
        _out = static_cast<alx::uint_64>(n);
        return true;
    }

    // parse_config — decode the varsolid hex blob; every failure names its key
    bool parse_config(const std::string& _hex, options& _out, std::string& _err) {
        const alx::bytes blob = alx::bytes::from_hex(_hex);
        if (blob.empty()) {
            _err = "config is not a hex string";
            return false;
        }
        alx::varmap v;
        if (!alx::varsolid::to_varmap(alx::bytes_view(blob), v)) {
            _err = "bad config blob";
            return false;
        }
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (!config_key(it.key())) {
                _err = "unknown config key: " + it.key();
                return false;
            }
        }
        using namespace proto::config;
        if (!take_string(v, kTransport, _out.transport, _err) ||
            !take_string(v, kEndpoint, _out.endpoint, _err) ||
            !take_string(v, kToken, _out.token, _err))
            return false;
        if (v.contain(kTrace) && !take_string(v, kTrace, _out.trace, _err)) return false;
        if (v.contain(kLogPath) && !take_string(v, kLogPath, _out.frame.log_path, _err)) return false;
        if (!take_number(v, kFrameMs, _out.frame.frame_ms, kFrameMsMin, kFrameMsMax, _err) ||
            !take_number(v, kFrameBytes, _out.frame.frame_bytes, kFrameBytesMin, kFrameBytesMax, _err) ||
            !take_number(v, kTrimBytes, _out.frame.trim_bytes, kTrimBytesMin, kTrimBytesMax, _err))
            return false;
        if (_out.transport != "local" && _out.transport != "tcp") {
            _err = "config key 'transport' must be local or tcp";
            return false;
        }
        if (_out.endpoint.empty() || _out.token.empty()) {
            _err = "config needs a non-empty 'endpoint' and 'token'";
            return false;
        }
        return true;
    }

    // pid — this process, for hello
    alx::uint_32 pid() {
#ifdef _WIN32
        return static_cast<alx::uint_32>(::_getpid());
#else
        return static_cast<alx::uint_32>(::getpid());
#endif
    }

    // trace_msg — one line per message: direction, type, id (payload left out on purpose)
    void trace_msg(const char* _dir, const alx::varmap& _m) {
        if (!g_trace.is_open()) return;
        proto::message msg;
        const bool ok = proto::parse(_m, msg);
        std::lock_guard<std::mutex> lk(g_trace_mx);
        g_trace << _dir << " " << (ok ? msg.type : std::string("?")) << " id=" << msg.id
                << " re=" << msg.reply_to << "\n";
        g_trace.flush();
    }

    // connect_client — create and open a transport, retrying while the server is not up yet
    alx::transmit* connect_client(const std::string& _type, const std::string& _endpoint) {
        for (int i = 0; i < kConnectTries; i++) {
            alx::transmit* t = alx::transmit::create(_type + "_client", _endpoint);
            if (t && t->open()) return t;
            delete t;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return nullptr;
    }

    // payload_text — string field of a payload, empty when absent
    std::string payload_text(const alx::varmap& _p, const char* _key) {
        if (!_p.contain(_key)) return std::string();
        return _p.value(_key).to<std::string>();
    }

    // usage — option summary on stderr
    void usage() {
        std::fprintf(stderr,
                     "axc-runner — script execution process for axc-gui\n"
                     "usage: axc-runner --config <hex>\n"
                     "  --config <hex>   varsolid blob of: transport, endpoint, token, trace,\n"
                     "                   frameMs (0 or 16..500), frameBytes (0 or 4 KiB..8 MiB),\n"
                     "                   trimBytes (0 or 1 KiB..1 MiB), logPath\n"
                     "  -h, --help       this text\n");
    }
} // namespace

int main(int _argc, char** _argv) {
    std::string config_hex;
    for (int i = 1; i < _argc; i++) {
        const std::string arg = _argv[i];
        if (arg == "--config") {
            if (i + 1 >= _argc) {
                std::fprintf(stderr, "axc-runner: --config needs a value\n");
                return 2;
            }
            config_hex = _argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "axc-runner: unknown option: %s\n", arg.c_str());
            usage();
            return 2;
        }
    }
    if (config_hex.empty()) {
        usage();
        return 2;
    }
    options opts;
    std::string err;
    if (!parse_config(config_hex, opts, err)) {
        std::fprintf(stderr, "axc-runner: %s\n", err.c_str());
        return 2;
    }
    if (!opts.trace.empty()) {
        g_trace.open(std::filesystem::u8path(opts.trace), std::ios::app);
        if (!g_trace) std::fprintf(stderr, "axc-runner: cannot write trace file %s\n", opts.trace.c_str());
    }

    alx::transmit* link = connect_client(opts.transport, opts.endpoint);
    if (!link) {
        std::fprintf(stderr, "axc-runner: cannot connect to %s (%s)\n", opts.endpoint.c_str(),
                     opts.transport.c_str());
        return 3;
    }

    alx::comm_ex comm(link);
    runner::control ctl;
    comm.data_recv.connect([&ctl](const alx::varmap& _m, alx::uint_64) { ctl.push(_m); });
    comm.mesg_prit.connect([](const std::string& _s) { std::fprintf(stderr, "axc-runner: %s\n", _s.c_str()); });
    comm.comm_flag.connect([&ctl](alx::uint_64, bool _on) {
        if (_on) return;
        ctl.set_link_lost(true);
        ctl.request_shutdown();
    });
    if (!comm.open()) {
        std::fprintf(stderr, "axc-runner: cannot start the comm worker\n");
        return 3;
    }

    runner::writer out(comm, opts.frame, kDefaultHighWater);
    out.set_trace([](const alx::varmap& _m) { trace_msg("->", _m); });
    const std::vector<std::string> caps = {"lossless-output", "position-scalar", "chunked-output"};
    // queue hello before the writer can report a bad log: hello stays the first message out
    out.send_control(proto::make_hello(opts.token, pid(), kRunnerVersion, core::version(), caps, kDefaultHighWater));
    out.start();

    bool welcomed = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kHandshakeMs);
    while (!welcomed && std::chrono::steady_clock::now() < deadline) {
        alx::varmap raw;
        if (!ctl.pop(raw, 100)) continue;
        trace_msg("<-", raw);
        proto::message m;
        if (!proto::parse(raw, m) || m.type != proto::kWelcome) continue;
        welcomed = m.payload.contain("ok") && m.payload.value("ok").to<bool>(false);
        if (!welcomed) {
            const std::string reason = payload_text(m.payload, "reason");
            std::fprintf(stderr, "axc-runner: handshake refused%s%s\n", reason.empty() ? "" : ": ",
                         reason.c_str());
        }
    }
    if (!welcomed) {
        std::fprintf(stderr, "axc-runner: handshake with the GUI failed or timed out\n");
        out.stop(false);
        comm.close();
        return 3;
    }

    std::thread engine([&ctl, &out] {
        runner::session sess(ctl, out);
        proto::exec_request req;
        alx::int_64 id = 0;
        while (ctl.take_exec(req, id)) {
            const bool console = req.console;
            sess.run(req);
            ctl.mark_exec_done(!console);
            if (!console) break;
        }
    });

    while (!ctl.exec_done() && !((ctl.shutdown_requested() || ctl.stop_requested()) && !ctl.busy())) {
        alx::varmap raw;
        if (!ctl.pop(raw, 100)) continue;
        trace_msg("<-", raw);

        proto::message m;
        if (!proto::parse(raw, m)) {
            out.send_control(proto::make_log("error", "dropped a malformed message"));
            continue;
        }
        if (m.type == proto::kPing) {
            out.send_control(proto::make_pong(m.id));
        } else if (m.type == proto::kPause) {
            ctl.request_pause();
        } else if (m.type == proto::kContinue || m.type == proto::kStep ||
                   m.type == proto::kSnapshot || m.type == proto::kView) {
            if (ctl.debug_active())
                ctl.push_debug(std::move(m));
            else
                out.send_control(proto::make_log("warn", "no paused run to answer " + m.type));
        } else if (m.type == proto::kSetBreakpoints) {
            ctl.push_breakpoints(std::move(m));
        } else if (m.type == proto::kStop) {
            out.interrupt();
            ctl.request_stop();
        } else if (m.type == proto::kInput) {
            ctl.push_input(payload_text(m.payload, "text"));
        } else if (m.type == proto::kExec) {
            proto::exec_request req;
            if (!proto::parse_exec(m.payload, req))
                out.send_control(proto::make_log("error", "exec names neither inline text nor a path"));
            else if (!ctl.queue_exec(std::move(req), m.id))
                out.send_control(proto::make_log("warn", "exec ignored: a run is already in progress"));
        } else if (m.type == proto::kReset) {
            ctl.request_reset();
        } else if (m.type == proto::kShutdown) {
            out.interrupt();
            ctl.request_shutdown();
        } else if (m.type == proto::kWelcome) {
            continue;
        } else {
            out.send_control(proto::make_log("warn", "unknown message type: " + m.type));
        }
    }

    // linger until the GUI acknowledges the exit; its shutdown proves everything before it was read
    const auto linger_end = std::chrono::steady_clock::now() + std::chrono::milliseconds(kLingerMs);
    while (!ctl.shutdown_requested() && !ctl.link_lost() &&
           std::chrono::steady_clock::now() < linger_end) {
        alx::varmap raw;
        if (!ctl.pop(raw, 50)) continue;
        trace_msg("<-", raw);
        proto::message m;
        if (!proto::parse(raw, m)) continue;
        if (m.type == proto::kShutdown) break;
        if (m.type == proto::kPing) out.send_control(proto::make_pong(m.id));
        if (m.type == proto::kStop) ctl.request_stop();
    }

    ctl.request_shutdown();
    engine.join();

    if (ctl.link_lost()) {
        // the peer is gone: close first so a writer blocked on a full queue is released
        comm.close();
        out.stop(false);
        return ctl.exit_code();
    }

    out.stop(true);
    comm.close();
    return ctl.exit_code();
}
