#ifndef AGUI_RUNNERPROCESS_H
#define AGUI_RUNNERPROCESS_H

#include "proto/message.h"

#include <acomm_ex.h>
#include <aplatform.h>

#include <QFile>
#include <QObject>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class QTimer;

// RunnerProcess — one axc-runner child and the comm_ex link that drives it.
// GUI listens (local_server, TCP fallback), the runner connects back with the token.
class RunnerProcess : public QObject {
    Q_OBJECT

public:
    // run_options — what the GUI hands the runner at spawn: output shaping, log, trace
    struct run_options {
        alx::uint_64 frame_ms = 16; // 0 = no time framing
        alx::uint_64 frame_bytes = 64u * 1024u; // 0 = no size cap
        alx::uint_64 trim_bytes = 64u * 1024u; // 0 = no trim
        bool log_file = true; // false leaves log_path empty
        QString log_path; // filled by Session when log_file is on
        QString trace_path; // empty = no protocol trace
    };

    explicit RunnerProcess(QObject* _parent = nullptr);
    ~RunnerProcess() override;

    // start — open the endpoint, spawn the runner, arm the handshake timer
    bool start(const QString& _runnerPath, const run_options& _options, alx::uint_64 _mem_bytes = 0);
    // send — queue one varmap for the writer thread; false when closing
    bool send(const alx::varmap& _m);
    void sendStop();
    void sendShutdown();
    void kill();
    void close();

    // pid — the runner process while it runs, 0 otherwise
    alx::uint_32 pid() const;
    bool isRunning() const;
    bool isReady() const { return m_ready; }
    QString lastError() const { return m_error; }
    // killReason — why a host-initiated kill happened; empty when the runner ended on its own
    QString killReason() const { return m_kill_reason; }
    QString endpoint() const { return m_endpoint; }

    // on_message — protocol events, on the GUI thread; on_raw — runner stdout/stderr bytes
    std::function<void(const proto::message&)> on_message;
    std::function<void(const QString&, bool)> on_raw;

signals:
    void ready(); // hello accepted, welcome sent
    void failed(const QString& _reason);
    void exited(int _code, bool _signaled, int _signal, const QString& _stderrTail);
    void log(const QString& _text);

protected:
    bool event(QEvent* _ev) override; // runs functors posted by the comm/process worker threads

private:
    void stop_timers();
    void deliver(alx::varmap _m);
    void trace(const char* _dir, const alx::varmap& _m);
    void writer_loop();
    void poll();
    void heartbeat();
    void set_error(const QString& _text);
    void emit_raw(const std::string& _data, bool _err);

private:
    alx::comm_ex* m_comm = nullptr; // owns the transport
    std::unique_ptr<alx::process_ctrl> m_ctrl;
    QString m_runner;
    QString m_endpoint;
    QString m_transport;
    std::string m_token;
    QString m_error;
    QString m_kill_reason;
    std::string m_stderr_tail;
    QFile m_trace;
    std::mutex m_tail_mx;

    std::thread m_writer;
    std::mutex m_wmx;
    std::condition_variable m_wcv;
    std::deque<alx::varmap> m_queue;
    bool m_wstopping = false;

    QTimer* m_poll = nullptr;
    QTimer* m_hello = nullptr;
    QTimer* m_beat = nullptr;
    QTimer* m_stop = nullptr;
    alx::int_64 m_ping_id = 0;
    int m_pings = 0;
    bool m_ready = false;
    bool m_reported = false;
    std::atomic<bool> m_closing{false};
};

#endif // AGUI_RUNNERPROCESS_H
