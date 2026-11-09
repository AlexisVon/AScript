#ifndef AGUI_SESSION_H
#define AGUI_SESSION_H

#include "RunnerProcess.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <vector>

// Session — one run from the GUI's point of view: launch, stream output, stop, report.
// States: idle → launching (spawn + handshake) → running (exec sent) → stopping → finished.
class Session : public QObject {
    Q_OBJECT

public:
    enum class state { idle,
                       launching,
                       running,
                       paused,
                       console, // long-lived console, engine alive between execs
                       stopping,
                       finished };

    explicit Session(QObject* _parent = nullptr);
    ~Session() override;

    // run — start a fresh runner for one script; false when it could not launch
    bool run(const QString& _code, const QString& _filePath, const QString& _cwd, bool _debug = false);
    void stop();
    void submitInput(const QString& _line);
    void pause_run();
    void continue_run();
    void step_over();
    void step_into();
    void step_out();
    // run_to_line — temporary breakpoint at _row of _path, then continue
    void run_to_line(const QString& _path, int _row);
    // request_snapshot — ask for a page of _ref's children while paused
    void request_snapshot(const QString& _ref, quint64 _ofst = 0);
    // request_view — ask for the view payload of the value at _ref while paused
    void request_view(const QString& _ref);
    // push_breakpoints — replace one file's breakpoints on a live debug runner
    void push_breakpoints(const QString& _path);
    // focus_thread — the paused thread the stack and variable views follow
    quint64 focusThread() const { return m_focus; }
    void set_focus_thread(quint64 _id);
    // start_console — long-lived runner; exec lines with console_exec, then close_console
    bool start_console(const QString& _cwd);
    bool console_exec(const QString& _code);
    void close_console();
    // reset_console — drop the console's engine state, keeping the process
    void reset_console();
    bool isRunning() const;
    alx::uint_32 pid() const;
    QString lastError() const;

    // console state and run options
    bool is_console() const { return m_console; }
    bool isPaused() const { return m_state == state::paused; }
    state current() const { return m_state; }
    // logPath — raw-byte session log of the last run, empty when it could not be opened
    QString logPath() const { return m_logPath; }
    // clear_log_path — drop the stored log path (a run that never started)
    void clear_log_path();
    QString snapshot_ref() const { return m_snapshot_ref; }
    // setRunnerPath — where axc-runner lives; default <appdir>/axc-runner
    void setRunnerPath(const QString& _path) { m_runnerPath = _path; }
    // set_breakpoints — breakpoints for the next run; source ids follow "file:<path>"/"buf:run"
    void set_breakpoints(proto::breakpoint_list _bps) { m_bps = std::move(_bps); }
    void set_stop_on_entry(bool _on) { m_stop_on_entry = _on; }
    void set_skip_traps(bool _on) { m_skip_traps = _on; }
    void set_multi_thread(bool _on) { m_multi_thread = _on; }
    void set_log_keep_days(int _days) { m_log_keep_days = _days; }
    void set_limits(alx::uint_32 _wall_ms, alx::uint_64 _mem_bytes) {
        m_wall_ms = _wall_ms;
        m_mem_bytes = _mem_bytes;
    }
    // set_run_options — output shaping and the log switch for the next start
    void set_run_options(const RunnerProcess::run_options& _opts) { m_run_options = _opts; }
    // set_trace_path — GUI protocol trace file; empty disables it
    void set_trace_path(const QString& _path) { m_trace_path = _path; }

signals:
    void outputText(const QString& _text);
    void inputRequested();
    void execResult(const QString& _value, qint64 _elapsedUs, bool _ok, int _errorType);
    void finished(bool _ok, const QString& _message);
    void logMessage(const QString& _text);
    void runnerStarted(quint64 _pid); // process is up, before exec/console lines
    void paused();
    void consoleReady();
    void breakpointHit(const QString& _file, int _row, int _col);
    void breakpointFrames(const QVariantList& _frames);
    void snapshotReady(const QString& _ref, quint64 _ofst, const QVariantList& _nodes, bool _more);
    void viewReady(const QString& _ref, bool _ok, const QString& _error, const QVariantMap& _view);
    void logPathChanged(const QString& _path); // session log of the new run; empty when logging is off
    // threadsChanged — one stop wave's captured threads; _known counts live engines
    void threadsChanged(const QVariantList& _threads, quint64 _stop, int _known, int _captured);
    // focusChanged — the focused thread's frames and root ref are ready
    void focusChanged();
    // trapReported — the trigger thread's trap payload, once per wave
    void trapReported(const QString& _label, const QString& _args);

private:
    // thread_entry — one captured thread of the current stop wave
    struct thread_entry {
        quint64 id = 0;
        qint64 parent = -1;
        bool trigger = false;
        QString label;
        QString trapArgs;
        std::vector<proto::stack_frame> frames;
    };

    void on_ready();
    void on_message(const proto::message& _m);
    void on_stopped(const alx::varmap& _payload);
    void apply_focus();
    QVariantList thread_list() const;
    QVariantList frames_list(const std::vector<proto::stack_frame>& _frames) const;
    void reset_wave();
    void on_snapshot(const alx::varmap& _payload);
    void on_view(const alx::varmap& _payload, alx::int_64 _id);
    void on_exited(int _code, bool _signaled, int _signal, const QString& _stderrTail);
    void finish(bool _ok, const QString& _message);
    void clear_temp();
    QString fresh_log();

private:
    RunnerProcess* m_runner = nullptr;
    state m_state = state::idle;
    proto::exec_request m_req;
    QString m_runnerPath;
    std::vector<proto::stack_frame> m_frames;
    QHash<quint64, thread_entry> m_threads;
    quint64 m_stop_id = 0;
    quint64 m_focus = 0;
    quint64 m_known = 1;
    bool m_resumed = true;
    bool m_announced = false;
    bool m_multi_thread = false;
    QString m_snapshot_ref;
    alx::int_64 m_view_seq = 0;
    RunnerProcess::run_options m_run_options;
    QString m_trace_path;
    QString m_logPath;
    proto::breakpoint_list m_bps;
    proto::breakpoint m_temp;
    bool m_has_temp = false;
    bool m_stop_on_entry = false;
    bool m_skip_traps = false;
    int m_log_keep_days = 7;
    bool m_console = false;
    bool m_console_closing = false;
    alx::uint_32 m_wall_ms = 0;
    alx::uint_64 m_mem_bytes = 0;
    bool m_ok = true;
    bool m_finished = false;
    bool m_exited_seen = false;
    bool m_stop_on_ready = false; // stop pressed before the exec command went out
    QString m_message;
};

#endif // AGUI_SESSION_H
