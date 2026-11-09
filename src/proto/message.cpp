#include "message.h"

#include <utility>

// Protocol helpers — every payload field is optional and read with a default
namespace proto {
    namespace {

        // text — string field, empty when absent or another type
        static std::string text(const alx::varmap& _m, const char* _key, const std::string& _def = std::string()) {
            if (!_m.contain(_key)) return _def;
            return _m.value(_key).to<std::string>(_def);
        }

        // integer — numeric field widened to int_64, _def when absent or not numeric
        static alx::int_64 integer(const alx::varmap& _m, const char* _key, alx::int_64 _def = 0) {
            if (!_m.contain(_key)) return _def;
            return _m.value(_key).to<alx::int_64>(_def);
        }

        // flag — bool field, _def when absent or another type
        static bool flag(const alx::varmap& _m, const char* _key, bool _def = false) {
            if (!_m.contain(_key)) return _def;
            return _m.value(_key).to<bool>(_def);
        }

        // sub — nested map field, a shared empty one when absent or another type
        static const alx::varmap& sub(const alx::varmap& _m, const char* _key) {
            static const alx::varmap kEmpty;
            if (!_m.contain(_key)) return kEmpty;
            return _m.value(_key).to<alx::varmap>(kEmpty);
        }

        // vector_of — vector field, null when absent or another type
        static const alx::varvec* vector_of(const alx::varmap& _m, const char* _key) {
            if (!_m.contain(_key)) return nullptr;
            const alx::variant& v = _m.value(_key);
            return v.is<alx::varvec>() ? &v.to<alx::varvec>() : nullptr;
        }

        // strings — string vector field, empty when absent or another type
        static std::vector<std::string> strings(const alx::varmap& _m, const char* _key) {
            static const alx::varvec kEmpty;
            std::vector<std::string> out;
            if (!_m.contain(_key)) return out;
            for (const auto& v : _m.value(_key).to<alx::varvec>(kEmpty))
                if (v.is<std::string>()) out.push_back(v.to<std::string>());
            return out;
        }

        // platform / arch — compile-time identity carried by hello
        static const char* platform() {
#ifdef _WIN32
            return "windows";
#elif defined(__APPLE__)
            return "darwin";
#elif defined(__linux__)
            return "linux";
#else
            return "unknown";
#endif
        }

        static const char* arch() {
#if defined(__x86_64__) || defined(_M_X64)
            return "x64";
#elif defined(__aarch64__) || defined(_M_ARM64)
            return "arm64";
#elif defined(__i386__) || defined(_M_IX86)
            return "x86";
#else
            return "unknown";
#endif
        }

    } // namespace

    alx::varmap make(const std::string& _type, alx::varmap _payload, alx::int_64 _id, alx::int_64 _re) {
        alx::varmap m;
        m[kVersionKey] = alx::variant(kVersion);
        m[kIdKey] = alx::variant(_id);
        m[kReplyToKey] = alx::variant(_re);
        m[kTypeKey] = alx::variant(_type);
        if (!_payload.empty()) m[kPayloadKey] = alx::variant(std::move(_payload));
        return m;
    }

    bool parse(const alx::varmap& _m, message& _out) {
        if (!_m.contain(kTypeKey) || !_m.contain(kVersionKey)) return false;
        if (integer(_m, kVersionKey, 0) != kVersion) return false;
        _out.type = text(_m, kTypeKey);
        if (_out.type.empty()) return false;
        _out.id = integer(_m, kIdKey);
        _out.reply_to = integer(_m, kReplyToKey);
        _out.payload = sub(_m, kPayloadKey);
        return true;
    }

    alx::varmap make_hello(const std::string& _token, alx::uint_32 _pid, const std::string& _runner_version,
                           const std::string& _engine_version, const std::vector<std::string>& _caps,
                           alx::uint_64 _queue_bytes) {
        alx::varmap p;
        p["proto"] = alx::variant(kVersion);
        p["token"] = alx::variant(_token);
        p["pid"] = alx::variant(alx::int_64(_pid));
        p["os"] = alx::variant(std::string(platform()));
        p["arch"] = alx::variant(std::string(arch()));
        p["runnerVersion"] = alx::variant(_runner_version);
        p["engineVersion"] = alx::variant(_engine_version);
        alx::varvec caps;
        for (const auto& c : _caps) caps.push_back(alx::variant(c));
        p["caps"] = alx::variant(std::move(caps));
        alx::varmap out;
        out["queueBytes"] = alx::variant(alx::int_64(_queue_bytes));
        p["output"] = alx::variant(std::move(out));
        return make(kHello, std::move(p));
    }

    alx::varmap make_output(const std::string& _stream, alx::uint_64 _seq, alx::bytes _data) {
        alx::varmap p;
        p["stream"] = alx::variant(_stream);
        p["seq"] = alx::variant(alx::int_64(_seq));
        p["data"] = alx::variant(std::move(_data));
        return make(kOutput, std::move(p));
    }

    alx::varmap make_compile_error(const std::string& _path, alx::uint_64 _row, alx::uint_64 _col,
                                   const std::string& _msg) {
        alx::varmap p;
        p["path"] = alx::variant(_path);
        p["row"] = alx::variant(alx::int_64(_row));
        p["col"] = alx::variant(alx::int_64(_col));
        p["msg"] = alx::variant(_msg);
        return make(kCompileError, std::move(p));
    }

    alx::varmap make_input_request(const std::string& _prompt) {
        alx::varmap p;
        if (!_prompt.empty()) p["prompt"] = alx::variant(_prompt);
        return make(kInputRequest, std::move(p));
    }

    alx::varmap make_result(bool _ok, const std::string& _value, alx::int_64 _elapsed_us, int _error,
                            const std::string& _error_name, const std::string& _text) {
        alx::varmap p;
        p["ok"] = alx::variant(_ok);
        p["value"] = alx::variant(_value);
        p["elapsedUs"] = alx::variant(_elapsed_us);
        p["error"] = alx::variant(alx::int_64(_error));
        p["errorName"] = alx::variant(_error_name);
        p["message"] = alx::variant(_text);
        p["lastOutputSeq"] = alx::variant(alx::int_64(0)); // stamped by the writer
        return make(kResult, std::move(p));
    }

    alx::varmap make_exited(alx::int_32 _code, bool _crashed, const std::string& _stderr_tail) {
        alx::varmap p;
        p["code"] = alx::variant(alx::int_64(_code));
        p["crashed"] = alx::variant(_crashed);
        p["stderrTail"] = alx::variant(_stderr_tail);
        p["lastOutputSeq"] = alx::variant(alx::int_64(0)); // stamped by the writer
        return make(kExited, std::move(p));
    }

    alx::varmap make_log(const std::string& _level, const std::string& _text) {
        alx::varmap p;
        p["level"] = alx::variant(_level);
        p["text"] = alx::variant(_text);
        return make(kLog, std::move(p));
    }

    alx::varmap make_pong(alx::int_64 _id) {
        return make(kPong, alx::varmap(), _id, _id);
    }

    alx::varmap make_welcome(const std::string& _session, bool _ok, const std::string& _reason) {
        alx::varmap p;
        p["sessionId"] = alx::variant(_session);
        p["ok"] = alx::variant(_ok);
        if (!_reason.empty()) p["reason"] = alx::variant(_reason);
        return make(kWelcome, std::move(p));
    }

    alx::varmap make_ping(alx::int_64 _id) {
        return make(kPing, alx::varmap(), _id);
    }

    alx::varmap make_stop(bool _force, alx::int_64 _id) {
        alx::varmap p;
        p["force"] = alx::variant(_force);
        return make(kStop, std::move(p), _id);
    }

    alx::varmap make_input(const std::string& _text, alx::int_64 _id) {
        alx::varmap p;
        p["text"] = alx::variant(_text);
        return make(kInput, std::move(p), _id);
    }

    alx::varmap make_reset(alx::int_64 _id) {
        return make(kReset, alx::varmap(), _id);
    }

    alx::varmap make_shutdown(alx::int_64 _id) {
        return make(kShutdown, alx::varmap(), _id);
    }

    alx::varmap make_exec(const exec_request& _r, alx::int_64 _id) {
        alx::varmap src;
        src["id"] = alx::variant(_r.source_id);
        src["path"] = alx::variant(_r.path);
        src["home"] = alx::variant(_r.home);
        src["text"] = alx::variant(_r.text);

        alx::varmap eng;
        eng["debug"] = alx::variant(_r.debug);
        eng["mtDebug"] = alx::variant(_r.mt_debug);
        eng["overflowCheck"] = alx::variant(_r.overflow_check);
        eng["maxStack"] = alx::variant(alx::int_64(_r.max_stack));
        eng["maxVecfill"] = alx::variant(alx::int_64(_r.max_vecfill));
        eng["parseDepth"] = alx::variant(alx::int_64(_r.parse_depth));
        alx::varvec paths;
        for (const auto& p : _r.search_paths) paths.push_back(alx::variant(p));
        eng["searchPaths"] = alx::variant(std::move(paths));

        alx::varmap hook;
        hook["interval"] = alx::variant(alx::int_64(_r.hook_interval));
        hook["skipTraps"] = alx::variant(_r.skip_traps);

        alx::varmap limits;
        limits["wallMs"] = alx::variant(alx::int_64(_r.wall_ms));

        alx::varvec bps;
        for (const auto& b : _r.breakpoints) {
            alx::varmap item;
            item["source"] = alx::variant(b.source);
            item["line"] = alx::variant(alx::int_64(b.line));
            item["enabled"] = alx::variant(b.enabled);
            if (!b.log.empty()) item["log"] = alx::variant(b.log);
            if (!b.condition.empty()) item["condition"] = alx::variant(b.condition);
            bps.push_back(alx::variant(std::move(item)));
        }

        alx::varmap p;
        p["source"] = alx::variant(std::move(src));
        p["mode"] = alx::variant(_r.mode);
        p["cwd"] = alx::variant(_r.cwd);
        p["engine"] = alx::variant(std::move(eng));
        p["hook"] = alx::variant(std::move(hook));
        p["limits"] = alx::variant(std::move(limits));
        p["console"] = alx::variant(_r.console);
        p["stopOnEntry"] = alx::variant(_r.stop_on_entry);
        p["breakpoints"] = alx::variant(std::move(bps));
        return make(kExec, std::move(p), _id);
    }

    bool parse_exec(const alx::varmap& _payload, exec_request& _out) {
        const alx::varmap& src = sub(_payload, "source");
        const alx::varmap& eng = sub(_payload, "engine");
        const alx::varmap& hook = sub(_payload, "hook");
        const alx::varmap& limits = sub(_payload, "limits");

        _out.source_id = text(src, "id");
        _out.path = text(src, "path");
        _out.home = text(src, "home");
        _out.text = text(src, "text");
        _out.mode = text(_payload, "mode");
        _out.cwd = text(_payload, "cwd");

        _out.debug = flag(eng, "debug");
        _out.mt_debug = flag(eng, "mtDebug");
        _out.overflow_check = flag(eng, "overflowCheck");
        _out.max_stack = static_cast<alx::uint_64>(integer(eng, "maxStack"));
        _out.max_vecfill = static_cast<alx::uint_64>(integer(eng, "maxVecfill"));
        _out.parse_depth = static_cast<alx::uint_64>(integer(eng, "parseDepth"));
        _out.search_paths = strings(eng, "searchPaths");

        _out.hook_interval = static_cast<alx::uint_64>(integer(hook, "interval"));
        _out.skip_traps = flag(hook, "skipTraps");
        _out.wall_ms = static_cast<alx::uint_32>(integer(limits, "wallMs"));
        _out.console = flag(_payload, "console");
        _out.stop_on_entry = flag(_payload, "stopOnEntry");
        if (const alx::varvec* bps = vector_of(_payload, "breakpoints")) {
            for (const auto& v : *bps) {
                if (!v.is<alx::varmap>()) continue;
                const alx::varmap& item = v.to<alx::varmap>();
                breakpoint b;
                b.source = text(item, "source");
                b.line = static_cast<alx::uint_64>(integer(item, "line"));
                b.enabled = flag(item, "enabled", true);
                b.log = text(item, "log");
                b.condition = text(item, "condition");
                if (!b.source.empty() && b.line) _out.breakpoints.push_back(std::move(b));
            }
        }

        if (_out.mode.empty()) _out.mode = _out.path.empty() ? "text" : "file";
        if (_out.mode == "text") return !_out.text.empty();
        return !_out.path.empty();
    }

    alx::varmap make_set_breakpoints(const std::string& _source, const std::vector<breakpoint>& _items,
                                     alx::int_64 _id) {
        alx::varmap p;
        p["source"] = alx::variant(_source);
        alx::varvec items;
        for (const auto& b : _items) {
            alx::varmap item;
            item["line"] = alx::variant(alx::int_64(b.line));
            item["enabled"] = alx::variant(b.enabled);
            if (!b.log.empty()) item["log"] = alx::variant(b.log);
            if (!b.condition.empty()) item["condition"] = alx::variant(b.condition);
            items.push_back(alx::variant(std::move(item)));
        }
        p["items"] = alx::variant(std::move(items));
        return make(kSetBreakpoints, std::move(p), _id);
    }

    bool parse_set_breakpoints(const alx::varmap& _payload, std::string& _source,
                               std::vector<breakpoint>& _items) {
        _source = text(_payload, "source");
        _items.clear();
        const alx::varvec* items = vector_of(_payload, "items");
        if (!items) return false;
        for (const auto& v : *items) {
            if (!v.is<alx::varmap>()) continue;
            const alx::varmap& item = v.to<alx::varmap>();
            breakpoint b;
            b.source = _source;
            b.line = static_cast<alx::uint_64>(integer(item, "line"));
            b.enabled = flag(item, "enabled", true);
            b.log = text(item, "log");
            b.condition = text(item, "condition");
            if (b.line) _items.push_back(std::move(b));
        }
        return !_source.empty();
    }

    alx::varmap make_pause(alx::int_64 _id) {
        return make(kPause, alx::varmap(), _id);
    }

    alx::varmap make_continue(alx::int_64 _id) {
        return make(kContinue, alx::varmap(), _id);
    }

    alx::varmap make_step(const std::string& _kind, alx::int_64 _id) {
        alx::varmap p;
        p["kind"] = alx::variant(_kind);
        return make(kStep, std::move(p), _id);
    }

    alx::varmap make_snapshot_request(const std::string& _ref, alx::uint_64 _limit, alx::uint_64 _ofst,
                                      alx::uint_64 _thread, alx::int_64 _id) {
        alx::varmap p;
        p["ref"] = alx::variant(_ref);
        p["limit"] = alx::variant(alx::int_64(_limit));
        p["ofst"] = alx::variant(alx::int_64(_ofst));
        p["thread"] = alx::variant(alx::int_64(_thread));
        return make(kSnapshot, std::move(p), _id);
    }

    alx::varmap make_view_request(const std::string& _ref, alx::uint_64 _thread, alx::int_64 _id) {
        alx::varmap p;
        p["ref"] = alx::variant(_ref);
        p["thread"] = alx::variant(alx::int_64(_thread));
        return make(kView, std::move(p), _id);
    }

    alx::varmap make_stopped(const stop_info& _s) {
        alx::varmap p;
        p["reason"] = alx::variant(_s.reason);
        p["thread"] = alx::variant(alx::int_64(_s.thread));
        p["parent"] = alx::variant(alx::int_64(_s.parent));
        p["trigger"] = alx::variant(alx::int_64(_s.trigger));
        p["stop"] = alx::variant(alx::int_64(_s.stop));
        p["known"] = alx::variant(alx::int_64(_s.known));
        p["lastOutputSeq"] = alx::variant(alx::int_64(0)); // stamped by the writer
        if (!_s.label.empty()) p["label"] = alx::variant(_s.label);
        if (!_s.trap_args.empty()) p["trapArgs"] = alx::variant(_s.trap_args);
        alx::varvec frames;
        for (const auto& f : _s.frames) {
            alx::varmap item;
            item["func"] = alx::variant(f.func);
            item["source"] = alx::variant(f.source);
            item["ref"] = alx::variant(f.ref);
            if (!f.alias.empty()) item["alias"] = alx::variant(f.alias);
            item["line"] = alx::variant(alx::int_64(f.line));
            item["col"] = alx::variant(alx::int_64(f.col));
            item["ofst"] = alx::variant(alx::int_64(f.ofst));
            if (f.internal) item["internal"] = alx::variant(true);
            frames.push_back(alx::variant(std::move(item)));
        }
        p["frames"] = alx::variant(std::move(frames));
        return make(kStopped, std::move(p));
    }

    alx::varmap make_snapshot(const std::string& _ref, alx::uint_64 _ofst, alx::uint_64 _thread,
                              const std::vector<snapshot_node>& _nodes, bool _more) {
        alx::varmap p;
        p["ref"] = alx::variant(_ref);
        p["ofst"] = alx::variant(alx::int_64(_ofst));
        p["thread"] = alx::variant(alx::int_64(_thread));
        p["more"] = alx::variant(_more);
        alx::varvec nodes;
        for (const auto& n : _nodes) {
            alx::varmap item;
            item["name"] = alx::variant(n.name);
            item["type"] = alx::variant(n.type);
            if (!n.value.empty()) item["value"] = alx::variant(n.value);
            if (!n.kind.empty()) item["kind"] = alx::variant(n.kind);
            if (!n.ref.empty()) item["ref"] = alx::variant(n.ref);
            if (!n.handle.empty()) item["handle"] = alx::variant(n.handle);
            if (n.shared) item["shared"] = alx::variant(true);
            if (n.has_children) item["hasChildren"] = alx::variant(true);
            nodes.push_back(alx::variant(std::move(item)));
        }
        p["nodes"] = alx::variant(std::move(nodes));
        return make(kSnapshot, std::move(p));
    }

    alx::varmap make_view(const std::string& _ref, bool _ok, const std::string& _error,
                          alx::variant&& _view, alx::uint_64 _thread, alx::int_64 _id) {
        alx::varmap p;
        p["ref"] = alx::variant(_ref);
        p["ok"] = alx::variant(_ok);
        p["thread"] = alx::variant(alx::int_64(_thread));
        if (!_error.empty()) p["error"] = alx::variant(_error);
        if (!_view.null()) p["view"] = std::move(_view);
        return make(kView, std::move(p), _id);
    }

} // namespace proto
