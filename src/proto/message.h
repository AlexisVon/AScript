#ifndef APROTO_MESSAGE_H
#define APROTO_MESSAGE_H

#include <avariant.h>

#include <string>
#include <vector>

// Wire protocol shared by axc-gui and axc-runner; no Qt, no engine.
// One message is one varmap, framed and checksummed by comm_ex:
//   {"v":1,"id":N,"re":M,"type":"...","payload":{...}}
namespace proto {

    // Protocol version; a mismatch fails the handshake instead of degrading
    constexpr alx::int_64 kVersion = 3;

    // Envelope keys
    constexpr const char* kVersionKey = "v";
    constexpr const char* kIdKey = "id";
    constexpr const char* kReplyToKey = "re";
    constexpr const char* kTypeKey = "type";
    constexpr const char* kPayloadKey = "payload";

    // Message types
    constexpr const char* kHello = "hello";
    constexpr const char* kWelcome = "welcome";
    constexpr const char* kExec = "exec";
    constexpr const char* kPause = "pause";
    constexpr const char* kContinue = "continue";
    constexpr const char* kStep = "step";
    constexpr const char* kSetBreakpoints = "setBreakpoints";
    constexpr const char* kSnapshot = "snapshot";
    constexpr const char* kView = "view";
    constexpr const char* kStop = "stop";
    constexpr const char* kInput = "input";
    constexpr const char* kPing = "ping";
    constexpr const char* kPong = "pong";
    constexpr const char* kReset = "reset";
    constexpr const char* kShutdown = "shutdown";
    constexpr const char* kOutput = "output";
    constexpr const char* kCompileError = "compileError";
    constexpr const char* kInputRequest = "inputRequest";
    constexpr const char* kStopped = "stopped";
    constexpr const char* kResult = "result";
    constexpr const char* kExited = "exited";
    constexpr const char* kLog = "log";

    // One decoded envelope
    struct message {
        alx::int_64 id = 0;
        alx::int_64 reply_to = 0;
        std::string type;
        alx::varmap payload;
    };

    // make — envelope varmap from its parts
    alx::varmap make(const std::string& _type, alx::varmap _payload = alx::varmap(),
                     alx::int_64 _id = 0, alx::int_64 _re = 0);

    // parse — decode an envelope; false when the version or the type is missing or wrong
    bool parse(const alx::varmap& _m, message& _out);

    // ── runner → gui ──────────────────────────────────────────────────────

    // make_hello — identity, versions and capabilities, sent before anything else
    alx::varmap make_hello(const std::string& _token, alx::uint_32 _pid,
                           const std::string& _runner_version, const std::string& _engine_version,
                           const std::vector<std::string>& _caps, alx::uint_64 _queue_bytes);

    // make_output — one lossless output chunk; _data is raw bytes, not text
    alx::varmap make_output(const std::string& _stream, alx::uint_64 _seq, alx::bytes _data);

    // make_compile_error — one compile diagnostic
    alx::varmap make_compile_error(const std::string& _path, alx::uint_64 _row, alx::uint_64 _col,
                                   const std::string& _msg);

    // make_input_request — the script asked for a line; prompt is optional
    alx::varmap make_input_request(const std::string& _prompt);

    // make_result — the run's outcome; _value is already a repr string.
    // lastOutputSeq is stamped by the writer when the event is queued, not here.
    alx::varmap make_result(bool _ok, const std::string& _value, alx::int_64 _elapsed_us,
                            int _error, const std::string& _error_name, const std::string& _text);

    // make_exited — the process is about to end; runner-side crashes never send this
    alx::varmap make_exited(alx::int_32 _code, bool _crashed, const std::string& _stderr_tail);

    // make_log — runner diagnostics, not script output
    alx::varmap make_log(const std::string& _level, const std::string& _text);

    // make_pong — heartbeat answer carrying the ping's id
    alx::varmap make_pong(alx::int_64 _id);

    // ── gui → runner ──────────────────────────────────────────────────────

    // make_welcome — the handshake answer; _ok false carries the reason
    alx::varmap make_welcome(const std::string& _session, bool _ok, const std::string& _reason = "");

    // make_ping — heartbeat request
    alx::varmap make_ping(alx::int_64 _id);

    // make_stop — ask the run to end; _force is advice for the parent, not the runner
    alx::varmap make_stop(bool _force, alx::int_64 _id = 0);

    // make_input — answer to inputRequest
    alx::varmap make_input(const std::string& _text, alx::int_64 _id = 0);

    // make_reset — drop the engine state before the next exec (console sessions)
    alx::varmap make_reset(alx::int_64 _id = 0);
    // make_shutdown — end the session; also the acknowledgement that lets the runner close
    alx::varmap make_shutdown(alx::int_64 _id = 0);

    // breakpoint — one source line the runner should stop at
    struct breakpoint {
        std::string source; // Source identity, e.g. "file:/abs/path" or the root's source id
        alx::uint_64 line = 0;
        bool enabled = true;
        std::string log; // non-empty: log this text and keep running (logpoint)
        std::string condition; // host-side predicate; empty = always
    };

    // breakpoint_list — every breakpoint the runner should honour
    using breakpoint_list = std::vector<breakpoint>;

    // stack_frame — one frame in a stopped event; metadata only, no values
    struct stack_frame {
        std::string func; // def name, "eval", or "?" for a block/loop frame
        std::string source; // Source identity of the frame
        std::string ref; // Snapshot ref of the frame ("f<engine index>" or "root")
        std::string alias; // Import alias when the frame comes from an embedded module
        alx::uint_64 line = 0;
        alx::uint_64 col = 0;
        alx::uint_64 ofst = 0; // 0-based byte offset of the statement
        bool internal = false; // the layer cannot be bound to an activation: no variables
    };

    // stop_info — one thread's arrival at a pause; every thread reports on its own
    struct stop_info {
        std::string reason; // trap, breakpoint, step, pause, entry, sync
        std::vector<stack_frame> frames;
        alx::uint_64 thread = 0; // 0 = main; children get 1, 2, ...
        alx::int_64 parent = -1; // creating thread id; -1 = main
        alx::uint_64 trigger = 0; // thread that started this stop wave
        alx::uint_64 stop = 0; // stop-wave id; a new id starts a new wave
        alx::uint_64 known = 1; // live engines: main + children
        std::string label; // short trap label, trigger thread only
        std::string trap_args; // full trap payload, trigger thread only
    };

    // snapshot_node — one variable/tree node; values are already rendered text
    struct snapshot_node {
        std::string name;
        std::string type; // int, float, string, bytes, map, vec, lst, handle, module, link, null
        std::string value; // rendered value for scalars; empty for expandable nodes
        std::string kind; // "", "handle", "module", "link"
        std::string ref; // continuation ref for children
        std::string handle; // hex form of an anyptr: "obj=.. ref=.. reg=.."
        bool shared = false;
        bool has_children = false;
    };

    // exec_request — one decoded exec command
    struct exec_request {
        std::string source_id; // Stable source identity chosen by the host
        std::string path; // File / .ax / .axp to run; empty for inline text
        std::string home; // Relative-import base for inline text
        std::string text; // Inline source
        std::string mode; // "text" | "file" | "ax"
        std::string cwd; // Working directory before the run

        bool debug = false; // Parse with position markers and fire debug events
        bool mt_debug = false; // Debug child engines created by $thd_create; needs debug
        bool overflow_check = false; // Raise instead of wrapping
        alx::uint_64 max_stack = 0; // 0 = engine default
        alx::uint_64 max_vecfill = 0; // 0 = engine default
        alx::uint_64 parse_depth = 0; // 0 = engine default
        std::vector<std::string> search_paths;
        alx::uint_64 hook_interval = 0; // 0 = no exec checkpoints
        bool skip_traps = false; // Ignore trap() breakpoints
        alx::uint_32 wall_ms = 0; // 0 = no watchdog
        bool console = false; // Keep the process (and the engine) alive for the next exec
        bool stop_on_entry = false; // Stop at the first statement
        std::vector<breakpoint> breakpoints;
    };

    // make_exec — encode an exec request
    alx::varmap make_exec(const exec_request& _r, alx::int_64 _id = 0);

    // parse_exec — decode an exec payload; false when it names neither text nor a path
    bool parse_exec(const alx::varmap& _payload, exec_request& _out);

    // make_set_breakpoints — replace the breakpoints of one source
    alx::varmap make_set_breakpoints(const std::string& _source, const std::vector<breakpoint>& _items,
                                     alx::int_64 _id = 0);
    // parse_set_breakpoints — decode a setBreakpoints payload
    bool parse_set_breakpoints(const alx::varmap& _payload, std::string& _source,
                               std::vector<breakpoint>& _items);

    // make_pause — stop at the next statement
    alx::varmap make_pause(alx::int_64 _id = 0);
    // make_continue — resume a paused run
    alx::varmap make_continue(alx::int_64 _id = 0);
    // make_step — "in" | "over" | "out"
    alx::varmap make_step(const std::string& _kind, alx::int_64 _id = 0);
    // make_snapshot_request — ask for a page of _ref's children while paused
    alx::varmap make_snapshot_request(const std::string& _ref, alx::uint_64 _limit = 0,
                                      alx::uint_64 _ofst = 0, alx::uint_64 _thread = 0,
                                      alx::int_64 _id = 0);
    // make_view_request — ask for the view payload of the value at _ref while paused
    alx::varmap make_view_request(const std::string& _ref, alx::uint_64 _thread = 0,
                                  alx::int_64 _id = 0);

    // make_stopped — one thread's pause report; lastOutputSeq is stamped by the writer
    alx::varmap make_stopped(const stop_info& _s);
    // make_snapshot — one page of a paused ref's children; _thread echoes the request
    alx::varmap make_snapshot(const std::string& _ref, alx::uint_64 _ofst, alx::uint_64 _thread,
                              const std::vector<snapshot_node>& _nodes, bool _more);
    // make_view — one view answer; _view is the link's opaque payload
    alx::varmap make_view(const std::string& _ref, bool _ok, const std::string& _error,
                          alx::variant&& _view, alx::uint_64 _thread = 0, alx::int_64 _id = 0);

} // namespace proto

#endif // APROTO_MESSAGE_H
