#include "RunnerProcess.h"

#include "proto/config.h"

#include <avarsolid.h>

#include <QCoreApplication>
#include <QEvent>
#include <QFileInfo>
#include <QStringList>
#include <QTimer>
#include <QUuid>

#include <random>
#include <vector>

namespace {
    // random_port — loopback port for each bind attempt; std::random keeps Qt 5.9 in play
    int random_port() {
        static std::mt19937 rng(std::random_device{}());
        return 20000 + static_cast<int>(rng() % 20000);
    }

    // qtext — UTF-8 bytes to a display string; invalid sequences become U+FFFD
    QString qtext(const std::string& _s) {
        return QString::fromUtf8(_s.data(), static_cast<int>(_s.size()));
    }

    // field_str — payload string field
    std::string field_str(const alx::varmap& _p, const char* _key) {
        if (!_p.contain(_key)) return std::string();
        return _p.value(_key).to<std::string>();
    }

    // field_int — payload numeric field
    alx::int_64 field_int(const alx::varmap& _p, const char* _key) {
        if (!_p.contain(_key)) return 0;
        return _p.value(_key).to<alx::int_64>(0);
    }

    const QEvent::Type k_func_event_type =
        static_cast<QEvent::Type>(QEvent::registerEventType());

    class FuncEvent : public QEvent {
    public:
        explicit FuncEvent(std::function<void()> _fn)
            : QEvent(k_func_event_type), fn(std::move(_fn)) {}

        std::function<void()> fn;
    };

    // post_queued — run _fn on _target's thread; invokeMethod's functor overload needs Qt 5.10
    void post_queued(QObject* _target, std::function<void()> _fn) {
        QCoreApplication::postEvent(_target, new FuncEvent(std::move(_fn)));
    }
} // namespace

RunnerProcess::RunnerProcess(QObject* _parent)
    : QObject(_parent) {
    m_poll = new QTimer(this);
    m_poll->setInterval(100);
    connect(m_poll, &QTimer::timeout, this, &RunnerProcess::poll);

    m_hello = new QTimer(this);
    m_hello->setSingleShot(true);
    connect(m_hello, &QTimer::timeout, this, [this] {
        if (m_ready) return;
        m_kill_reason = QStringLiteral("runner 握手超时，已终止");
        emit failed(m_kill_reason);
        kill();
    });

    m_beat = new QTimer(this);
    m_beat->setInterval(2000);
    connect(m_beat, &QTimer::timeout, this, &RunnerProcess::heartbeat);

    m_stop = new QTimer(this);
    m_stop->setSingleShot(true);
    connect(m_stop, &QTimer::timeout, this, [this] {
        if (m_reported) return;
        m_kill_reason = QStringLiteral("runner 未在时限内退出，已强制终止");
        emit log(m_kill_reason);
        kill();
    });
}

RunnerProcess::~RunnerProcess() {
    close();
}

bool RunnerProcess::start(const QString& _runnerPath, const run_options& _options, alx::uint_64 _mem_bytes) {
    close();
    m_closing.store(false);
    m_ready = false;
    m_reported = false;
    m_pings = 0;
    m_error.clear();
    m_kill_reason.clear();
    {
        std::lock_guard<std::mutex> lk(m_tail_mx);
        m_stderr_tail.clear();
    }

    QFileInfo runner(_runnerPath);
#ifdef Q_OS_WIN
    // the default path has no extension; probe the .exe sibling on Windows
    if (!runner.isFile() && !_runnerPath.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive))
        runner = QFileInfo(_runnerPath + QStringLiteral(".exe"));
#endif
    if (!runner.isFile() || !runner.isExecutable()) {
        set_error(QStringLiteral("找不到可执行的 runner: ") + _runnerPath);
        return false;
    }
    m_runner = runner.absoluteFilePath();

    if (!_options.trace_path.isEmpty()) {
        m_trace.setFileName(_options.trace_path);
        // truncate once, then append: the runner writes the same file with O_APPEND
        if (!m_trace.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Append))
            emit log(QStringLiteral("无法写入协议跟踪文件: ") + _options.trace_path);
    }
    m_token = QUuid::createUuid().toRfc4122().toHex().toStdString();

    alx::transmit* trans = nullptr;
    for (int i = 0; i < 10 && !trans; ++i) {
        const int port = random_port();
        m_endpoint = QStringLiteral("127.0.0.1:") + QString::number(port);

        m_transport = QStringLiteral("local");
        trans = alx::transmit::create("local_server", m_endpoint.toStdString());
        if (trans && trans->open()) break;
        delete trans;
        trans = nullptr;

        m_transport = QStringLiteral("tcp");
        trans = alx::transmit::create("tcp_server", m_endpoint.toStdString());
        if (trans && trans->open()) break;
        delete trans;
        trans = nullptr;
    }
    if (!trans) {
        set_error(QStringLiteral("无法监听本地端口"));
        return false;
    }

    m_comm = new alx::comm_ex(trans);
    m_comm->data_recv.connect([this](const alx::varmap& _m, alx::uint_64) {
        if (m_closing.load()) return;
        auto msg = std::make_shared<alx::varmap>(_m);
        post_queued(this, [this, msg] { deliver(std::move(*msg)); });
    });
    m_comm->mesg_prit.connect([this](const std::string& _s) {
        if (m_closing.load()) return;
        const QString text = qtext(_s);
        post_queued(this, [this, text] { emit log(text); });
    });
    m_comm->comm_flag.connect([this](alx::uint_64, bool _on) {
        if (_on || m_closing.load()) return;
        post_queued(this, [this] { emit log(QStringLiteral("runner 连接已断开")); });
    });
    if (!m_comm->open()) {
        set_error(QStringLiteral("comm 工作线程启动失败"));
        delete m_comm;
        m_comm = nullptr;
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(m_wmx);
        m_wstopping = false;
    }
    m_writer = std::thread([this] { writer_loop(); });

    m_ctrl = std::make_unique<alx::process_ctrl>(m_runner.toStdString());
    m_ctrl->sig_output.connect([this](const std::string& _s) { emit_raw(_s, false); });
    m_ctrl->sig_errput.connect([this](const std::string& _s) { emit_raw(_s, true); });

    alx::varmap cfg;
    cfg[proto::config::kTransport] = alx::variant(m_transport.toStdString());
    cfg[proto::config::kEndpoint] = alx::variant(m_endpoint.toStdString());
    cfg[proto::config::kToken] = alx::variant(m_token);
    cfg[proto::config::kFrameMs] = alx::variant(alx::int_64(_options.frame_ms));
    cfg[proto::config::kFrameBytes] = alx::variant(alx::int_64(_options.frame_bytes));
    cfg[proto::config::kTrimBytes] = alx::variant(alx::int_64(_options.trim_bytes));
    if (!_options.log_path.isEmpty())
        cfg[proto::config::kLogPath] = alx::variant(_options.log_path.toStdString());
    if (!_options.trace_path.isEmpty())
        cfg[proto::config::kTrace] = alx::variant(_options.trace_path.toStdString());
    const std::vector<std::string> args = {"--config", alx::varsolid::to_bytes(cfg).to_hex()};
    // kill_on_parent_exit: an IDE crash must not leave the runner (and its script) behind
    if (!m_ctrl->start(args, alx::process_ctrl::process_limits(_mem_bytes, true))) {
        set_error(QStringLiteral("无法启动 runner: ") + m_runner);
        close();
        return false;
    }

    m_hello->start(5000);
    m_poll->start(100);
    m_beat->start(2000);
    return true;
}

bool RunnerProcess::send(const alx::varmap& _m) {
    if (m_closing.load() || !m_comm) return false;
    {
        std::lock_guard<std::mutex> lk(m_wmx);
        if (m_wstopping) return false;
        m_queue.push_back(_m);
    }
    m_wcv.notify_all();
    trace("->", _m);
    return true;
}

void RunnerProcess::sendStop() {
    if (!send(proto::make_stop(false))) return;
    if (!m_stop->isActive()) m_stop->start(2000);
}

void RunnerProcess::sendShutdown() {
    if (!send(proto::make_shutdown())) return;
    if (!m_stop->isActive()) m_stop->start(3000);
}

void RunnerProcess::kill() {
    if (m_ctrl) m_ctrl->kill();
}

void RunnerProcess::close() {
    m_closing.store(true);
    stop_timers();
    if (m_ctrl) m_ctrl->kill();
    if (m_comm) m_comm->close();
    {
        std::lock_guard<std::mutex> lk(m_wmx);
        m_wstopping = true;
        m_queue.clear();
    }
    m_wcv.notify_all();
    if (m_writer.joinable()) m_writer.join();
    delete m_comm;
    m_comm = nullptr;
    m_ctrl.reset();
    m_ready = false;
}

alx::uint_32 RunnerProcess::pid() const {
    return m_ctrl ? m_ctrl->pid() : 0;
}

bool RunnerProcess::isRunning() const {
    if (!m_ctrl || m_reported) return false;
    return !m_ctrl->status().exited;
}

bool RunnerProcess::event(QEvent* _ev) {
    if (_ev->type() == k_func_event_type) {
        static_cast<FuncEvent*>(_ev)->fn();
        return true;
    }
    return QObject::event(_ev);
}

void RunnerProcess::stop_timers() {
    if (m_poll) m_poll->stop();
    if (m_hello) m_hello->stop();
    if (m_beat) m_beat->stop();
    if (m_stop) m_stop->stop();
}

void RunnerProcess::deliver(alx::varmap _m) {
    if (m_closing.load()) return;
    proto::message msg;
    if (!proto::parse(_m, msg)) {
        emit log(QStringLiteral("收到了无法解析的消息"));
        return;
    }
    if (msg.type == proto::kHello) {
        const bool ok = field_int(msg.payload, "proto") == proto::kVersion &&
                        field_str(msg.payload, "token") == m_token;
        if (!ok) {
            set_error(QStringLiteral("runner 握手被拒绝（token/协议不匹配）"));
            emit failed(m_error);
            kill();
            return;
        }
        send(proto::make_welcome(QStringLiteral("gui").toStdString(), true));
        m_hello->stop();
        m_ready = true;
        emit ready();
        return;
    }
    if (msg.type == proto::kPong) {
        m_pings = 0;
        return;
    }
    trace("<-", _m);
    if (on_message) on_message(msg);
}

void RunnerProcess::trace(const char* _dir, const alx::varmap& _m) {
    if (!m_trace.isOpen()) return;
    proto::message msg;
    const bool ok = proto::parse(_m, msg);
    m_trace.write(QByteArray(_dir) + " " + (ok ? msg.type.c_str() : "?") + " id=" +
                  QByteArray::number(static_cast<qint64>(msg.id)) + "\n");
    m_trace.flush();
}

void RunnerProcess::writer_loop() {
    for (;;) {
        alx::varmap m;
        {
            std::unique_lock<std::mutex> lk(m_wmx);
            m_wcv.wait(lk, [this] { return m_wstopping || !m_queue.empty(); });
            if (m_queue.empty()) break;
            m = std::move(m_queue.front());
            m_queue.pop_front();
        }
        if (m_comm) m_comm->data_send(m, 0, false);
    }
}

void RunnerProcess::poll() {
    if (!m_ctrl || m_reported) return;
    const alx::process_ctrl::wait_status st = m_ctrl->status();
    if (!st.exited) return;
    m_reported = true;
    stop_timers();

    // status() does not reap: kill() takes the tree down and reaps the child
    m_ctrl->kill();
    if (m_comm) m_comm->close();

    std::string tail;
    {
        std::lock_guard<std::mutex> lk(m_tail_mx);
        tail = m_stderr_tail;
    }
    int code = st.exit_code;
    bool signaled = false;
    int sig = 0;
#ifndef _WIN32
    signaled = st.signaled;
    sig = st.term_sig;
#endif
    emit exited(code, signaled, sig, qtext(tail));
}

void RunnerProcess::heartbeat() {
    if (!m_ready || !m_comm) return;
    if (++m_pings > 3) {
        m_kill_reason = QStringLiteral("runner 心跳丢失，已强制终止");
        emit log(m_kill_reason);
        kill();
        return;
    }
    send(proto::make_ping(++m_ping_id));
}

void RunnerProcess::set_error(const QString& _text) {
    m_error = _text;
    emit log(_text);
}

void RunnerProcess::emit_raw(const std::string& _data, bool _err) {
    if (m_closing.load()) return;
    if (_err) {
        std::lock_guard<std::mutex> lk(m_tail_mx);
        m_stderr_tail += _data;
        if (m_stderr_tail.size() > 4096) m_stderr_tail.erase(0, m_stderr_tail.size() - 4096);
    }
    // runner diagnostics are prefixed; keep them out of the script's output stream
    QString out;
    if (_err) {
        const QStringList lines = qtext(_data).split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size(); ++i) {
            const QString& line = lines[i];
            if (line.startsWith(QLatin1String("axc-runner: "))) {
                post_queued(this, [this, line] { emit log(line); });
                continue;
            }
            out += line;
            if (i + 1 < lines.size()) out += QLatin1Char('\n');
        }
    } else {
        out = qtext(_data);
    }
    if (out.isEmpty()) return;
    post_queued(this, [this, out, _err] {
        if (on_raw) on_raw(out, _err);
    });
}
