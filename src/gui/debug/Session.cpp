#include "Session.h"

#include "Settings.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>

namespace {
    // str_field / int_field / bool_field — payload readers with defaults
    std::string str_field(const alx::varmap& _p, const char* _key) {
        if (!_p.contain(_key)) return std::string();
        return _p.value(_key).to<std::string>();
    }

    alx::int_64 int_field(const alx::varmap& _p, const char* _key, alx::int_64 _def = 0) {
        if (!_p.contain(_key)) return _def;
        return _p.value(_key).to<alx::int_64>(_def);
    }

    bool bool_field(const alx::varmap& _p, const char* _key) {
        if (!_p.contain(_key)) return false;
        return _p.value(_key).to<bool>(false);
    }

    // to_qvariant — protocol value to Qt containers; bytes stay QByteArray
    QVariant to_qvariant(const alx::variant& _v) {
        if (_v.null()) return {};
        if (_v.is<bool>()) return _v.to<bool>();
        if (_v.is<alx::int_64>()) return static_cast<qlonglong>(_v.to<alx::int_64>());
        if (_v.is<alx::uint_64>()) return static_cast<qulonglong>(_v.to<alx::uint_64>());
        if (_v.is<double>()) return _v.to<double>();
        if (_v.is<std::string>()) return QString::fromStdString(_v.to<std::string>());
        if (_v.is<alx::bytes>()) {
            const alx::bytes& b = _v.to<alx::bytes>();
            return QByteArray(reinterpret_cast<const char*>(b.data()), static_cast<int>(b.size()));
        }
        if (_v.is<alx::varmap>()) {
            QVariantMap m;
            const alx::varmap& src = _v.to<alx::varmap>();
            for (auto it = src.begin(); it != src.end(); ++it)
                m[QString::fromStdString(it.key())] = to_qvariant(it.value());
            return m;
        }
        if (_v.is<alx::varvec>()) {
            QVariantList l;
            for (const auto& e : _v.to<alx::varvec>()) l.append(to_qvariant(e));
            return l;
        }
        if (_v.is<alx::varlst>()) {
            QVariantList l;
            for (const auto& e : _v.to<alx::varlst>()) l.append(to_qvariant(e));
            return l;
        }
        return {};
    }

    // display_path — source id to an editor path; empty when there is no file to open
    QString display_path(const std::string& _source) {
        const QString s = QString::fromStdString(_source);
        if (s.startsWith(QLatin1String("file:"))) return s.mid(5);
        return QString();
    }
} // namespace

Session::Session(QObject* _parent)
    : QObject(_parent) {
    m_runnerPath = QCoreApplication::applicationDirPath() + QStringLiteral("/axc-runner");
    m_runner = new RunnerProcess(this);
    m_runner->on_message = [this](const proto::message& _m) { on_message(_m); };
    m_runner->on_raw = [this](const QString& _text, bool) { emit outputText(_text); };
    connect(m_runner, &RunnerProcess::ready, this, &Session::on_ready);
    connect(m_runner, &RunnerProcess::failed, this, [this](const QString& _reason) {
        emit logMessage(_reason);
        finish(false, _reason);
    });
    connect(m_runner, &RunnerProcess::exited, this, &Session::on_exited);
    connect(m_runner, &RunnerProcess::log, this, &Session::logMessage);
}

Session::~Session() {
    if (m_runner) m_runner->close();
}

bool Session::run(const QString& _code, const QString& _filePath, const QString& _cwd, bool _debug) {
    if (isRunning()) {
        emit logMessage(QStringLiteral("已有运行中的会话"));
        return false;
    }
    // drop week-old session logs so the temp dir does not grow without bound
    const QFileInfoList stale =
        QDir(QDir::tempPath()).entryInfoList({QStringLiteral("axc-session-*.log")}, QDir::Files);
    for (const auto& fi : stale)
        if (m_log_keep_days > 0 &&
            fi.lastModified().daysTo(QDateTime::currentDateTime()) > m_log_keep_days)
            QFile::remove(fi.absoluteFilePath());

    clear_log_path();

    m_req = proto::exec_request();
    if (!_code.isEmpty()) {
        m_req.mode = "text";
        m_req.text = _code.toStdString();
        // a dirty file keeps its path identity, so its breakpoints and relative imports resolve
        if (!_filePath.isEmpty()) {
            const QFileInfo fi(_filePath);
            const QString canonical =
                fi.canonicalFilePath().isEmpty() ? fi.absoluteFilePath() : fi.canonicalFilePath();
            m_req.source_id = "file:" + canonical.toStdString();
            m_req.home = fi.absolutePath().toStdString();
        } else {
            m_req.source_id = "buf:run";
        }
    } else if (!_filePath.isEmpty()) {
        const QFileInfo fi(_filePath);
        m_req.path = fi.absoluteFilePath().toStdString();
        m_req.mode = fi.suffix() == QLatin1String("ax") ? "ax" : "file";
        if (m_req.mode == "file") m_req.source_id = "file:" + fi.canonicalFilePath().toStdString();
    } else {
        emit logMessage(QStringLiteral("没有可执行的脚本"));
        return false;
    }
    for (const QString& p : Settings::userLibraryDirs())
        m_req.search_paths.push_back(p.toStdString());
    m_req.debug = _debug || m_stop_on_entry || !m_bps.empty();
    m_req.mt_debug = m_multi_thread;
    m_req.stop_on_entry = m_stop_on_entry;
    m_req.skip_traps = m_skip_traps;
    m_req.breakpoints = m_bps;
    m_req.cwd = _cwd.toStdString();
    m_req.wall_ms = m_wall_ms;

    m_logPath = fresh_log();
    emit logPathChanged(m_logPath);
    m_run_options.log_path = m_logPath;
    m_run_options.trace_path = m_trace_path;

    m_console = false;
    m_console_closing = false;
    m_ok = true;
    m_finished = false;
    m_exited_seen = false;
    m_stop_on_ready = false;
    m_message.clear();
    reset_wave();
    m_state = state::launching;

    if (!m_runner->start(m_runnerPath, m_run_options, m_mem_bytes)) {
        m_state = state::idle;
        clear_log_path();
        emit logMessage(m_runner->lastError());
        return false;
    }
    return true;
}

void Session::stop() {
    if (!isRunning()) return;
    if (m_console) {
        close_console();
        return;
    }
    if (m_state == state::launching) {
        // exec has not gone out yet; ask the runner to shut down instead
        m_stop_on_ready = true;
        m_state = state::stopping;
        return;
    }
    m_state = state::stopping;
    emit logMessage(QStringLiteral("正在停止…"));
    m_runner->sendStop();
}

void Session::submitInput(const QString& _line) {
    if (!isRunning()) return;
    m_runner->send(proto::make_input(_line.toStdString()));
}

void Session::pause_run() {
    if (m_state != state::running) return;
    if (!m_req.debug) {
        emit logMessage(QStringLiteral("当前运行没有开启调试，无法暂停"));
        return;
    }
    m_runner->send(proto::make_pause());
}

void Session::continue_run() {
    if (m_state != state::paused) return;
    clear_temp();
    m_resumed = true;
    m_state = state::running;
    m_runner->send(proto::make_continue());
}

void Session::step_over() {
    if (m_state != state::paused || m_focus != 0) return;
    clear_temp();
    m_resumed = true;
    m_state = state::running;
    m_runner->send(proto::make_step("over"));
}

void Session::step_into() {
    if (m_state != state::paused || m_focus != 0) return;
    clear_temp();
    m_resumed = true;
    m_state = state::running;
    m_runner->send(proto::make_step("in"));
}

void Session::step_out() {
    if (m_state != state::paused || m_focus != 0) return;
    clear_temp();
    m_resumed = true;
    m_state = state::running;
    m_runner->send(proto::make_step("out"));
}

void Session::run_to_line(const QString& _path, int _row) {
    if (m_state != state::paused || m_focus != 0 || _path.isEmpty() || _row <= 0) return;
    const QFileInfo fi(_path);
    const QString canonical = fi.canonicalFilePath().isEmpty() ? fi.absoluteFilePath() : fi.canonicalFilePath();
    m_temp.source = "file:" + canonical.toStdString();
    m_temp.line = static_cast<alx::uint_64>(_row);
    m_temp.enabled = true;
    m_has_temp = true;
    proto::breakpoint_list items;
    for (const auto& b : m_bps)
        if (b.source == m_temp.source) items.push_back(b);
    items.push_back(m_temp);
    m_runner->send(proto::make_set_breakpoints(m_temp.source, items));
    m_resumed = true;
    m_state = state::running;
    m_runner->send(proto::make_continue());
}

void Session::request_snapshot(const QString& _ref, quint64 _ofst) {
    if (m_state != state::paused || _ref.isEmpty()) return;
    m_runner->send(proto::make_snapshot_request(_ref.toStdString(), 0,
                                                static_cast<alx::uint_64>(_ofst), m_focus));
}

void Session::request_view(const QString& _ref) {
    if (m_state != state::paused || _ref.isEmpty()) return;
    m_runner->send(proto::make_view_request(_ref.toStdString(), m_focus, ++m_view_seq));
}

void Session::push_breakpoints(const QString& _path) {
    if (!isRunning() || _path.isEmpty()) return;
    if (!m_req.debug) {
        emit logMessage(QStringLiteral("当前运行未开启调试，断点仅对下次运行生效"));
        return;
    }
    if (m_state == state::launching) { // exec has not gone out yet; it carries the list
        m_req.breakpoints = m_bps;
        return;
    }
    const QFileInfo fi(_path);
    const QString canonical = fi.canonicalFilePath().isEmpty() ? fi.absoluteFilePath() : fi.canonicalFilePath();
    const std::string source = "file:" + canonical.toStdString();
    proto::breakpoint_list items;
    for (const auto& b : m_bps)
        if (b.source == source) items.push_back(b);
    m_runner->send(proto::make_set_breakpoints(source, items));
}

void Session::set_focus_thread(quint64 _id) {
    if (m_state != state::paused) return;
    const auto it = m_threads.find(_id);
    if (it == m_threads.end() || m_focus == _id) return;
    m_focus = _id;
    ++m_view_seq; // drop view answers still in flight for the previous focus
    apply_focus();
}

bool Session::start_console(const QString& _cwd) {
    if (isRunning()) return false;
    m_logPath = fresh_log();
    emit logPathChanged(m_logPath);
    m_run_options.log_path = m_logPath;
    m_run_options.trace_path = m_trace_path;

    m_req = proto::exec_request();
    m_req.mode = "text";
    m_req.console = true;
    m_req.cwd = _cwd.toStdString();
    for (const QString& p : Settings::userLibraryDirs())
        m_req.search_paths.push_back(p.toStdString());
    m_console = true;
    m_console_closing = false;
    m_ok = true;
    m_finished = false;
    m_exited_seen = false;
    m_message.clear();
    reset_wave();
    m_state = state::launching;
    if (!m_runner->start(m_runnerPath, m_run_options, m_mem_bytes)) {
        m_state = state::idle;
        m_console = false;
        clear_log_path();
        emit logMessage(m_runner->lastError());
        return false;
    }
    return true;
}

bool Session::console_exec(const QString& _code) {
    if (m_state != state::console || _code.isEmpty()) return false;
    proto::exec_request req = m_req;
    req.text = _code.toStdString();
    req.source_id = "buf:repl";
    req.console = true;
    req.wall_ms = m_wall_ms;
    if (!m_runner->send(proto::make_exec(req))) return false;
    m_state = state::running;
    return true;
}

void Session::close_console() {
    if (!m_console || m_state == state::stopping) return;
    m_console_closing = true;
    m_state = state::stopping;
    m_runner->sendShutdown();
}

void Session::reset_console() {
    if (!m_console || !isRunning()) return;
    m_runner->send(proto::make_reset());
}

bool Session::isRunning() const {
    return m_state == state::launching || m_state == state::running || m_state == state::paused ||
           m_state == state::console || m_state == state::stopping;
}

alx::uint_32 Session::pid() const {
    return m_runner ? m_runner->pid() : 0;
}

QString Session::lastError() const {
    return m_runner ? m_runner->lastError() : QString();
}

void Session::on_ready() {
    if (m_console) {
        m_state = state::console;
        emit runnerStarted(m_runner->pid());
        emit consoleReady();
        return;
    }
    if (m_stop_on_ready) {
        m_runner->sendShutdown();
        finish(false, QStringLiteral("已停止"));
        return;
    }
    emit runnerStarted(m_runner->pid());
    if (m_state != state::launching) return;
    if (!m_runner->send(proto::make_exec(m_req))) {
        finish(false, QStringLiteral("无法把执行命令发给 runner"));
        return;
    }
    m_state = state::running;
}

void Session::on_message(const proto::message& _m) {
    const alx::varmap& p = _m.payload;
    if (_m.type == proto::kOutput) {
        if (!p.contain("data")) return;
        const alx::bytes& data = p.value("data").to<alx::bytes>();
        if (!data.size()) return;
        emit outputText(QString::fromUtf8(reinterpret_cast<const char*>(data.data()),
                                          static_cast<int>(data.size())));
    } else if (_m.type == proto::kCompileError) {
        emit outputText(QStringLiteral("%1:%2: %3\n")
                            .arg(int_field(p, "row"))
                            .arg(int_field(p, "col"))
                            .arg(QString::fromStdString(str_field(p, "msg"))));
    } else if (_m.type == proto::kInputRequest) {
        emit inputRequested();
    } else if (_m.type == proto::kStopped) {
        on_stopped(p);
    } else if (_m.type == proto::kSnapshot) {
        on_snapshot(p);
    } else if (_m.type == proto::kView) {
        on_view(p, _m.id);
    } else if (_m.type == proto::kResult) {
        const bool ok = bool_field(p, "ok");
        if (m_console) {
            const QString value = QString::fromStdString(str_field(p, "value"));
            const QString message = QString::fromStdString(str_field(p, "message"));
            m_ok = ok;
            m_message = ok ? QString() : (message.isEmpty() ? QString::fromStdString(str_field(p, "errorName")) : message);
            m_state = state::console;
            emit execResult(value.isEmpty() ? QStringLiteral("?") : value, int_field(p, "elapsedUs"), ok,
                            static_cast<int>(int_field(p, "error")));
            return;
        }
        const QString value = QString::fromStdString(str_field(p, "value"));
        const QString message = QString::fromStdString(str_field(p, "message"));
        m_ok = ok;
        m_message = ok ? QString() : (message.isEmpty() ? QString::fromStdString(str_field(p, "errorName")) : message);
        emit execResult(value.isEmpty() ? QStringLiteral("?") : value, int_field(p, "elapsedUs"), ok,
                        static_cast<int>(int_field(p, "error")));
    } else if (_m.type == proto::kExited) {
        m_exited_seen = true;
        finish(m_ok, m_message);
        m_runner->sendShutdown();
    } else if (_m.type == proto::kLog) {
        const QString text = QString::fromStdString(str_field(p, "text"));
        if (!text.isEmpty()) emit logMessage(QStringLiteral("[runner] ") + text);
    }
}

void Session::on_stopped(const alx::varmap& _p) {
    if (m_state == state::finished || m_state == state::stopping || m_state == state::console)
        return;
    const quint64 thread = static_cast<quint64>(int_field(_p, "thread"));
    const qint64 parent = static_cast<qint64>(int_field(_p, "parent", -1));
    const quint64 stop = static_cast<quint64>(int_field(_p, "stop"));
    const quint64 trigger = static_cast<quint64>(int_field(_p, "trigger"));
    const quint64 known = static_cast<quint64>(int_field(_p, "known"));
    const QString reason = QString::fromStdString(str_field(_p, "reason"));
    if (stop < m_stop_id) return; // a straggler from an older wave
    if (stop != m_stop_id) {
        m_stop_id = stop;
        m_threads.clear();
        m_focus = trigger;
        m_known = known ? known : 1;
        m_resumed = false;
        m_announced = false;
        m_state = state::paused;
    } else if (m_resumed) {
        return; // a report that was already in flight when the wave resumed
    } else if (known > m_known) {
        m_known = known;
    }

    thread_entry& t = m_threads[thread];
    t.id = thread;
    t.parent = parent;
    t.trigger = (thread == trigger);
    const QString label = QString::fromStdString(str_field(_p, "label"));
    if (!label.isEmpty()) t.label = label;
    const QString args = QString::fromStdString(str_field(_p, "trapArgs"));
    if (!args.isEmpty()) t.trapArgs = args;
    t.frames.clear();
    if (_p.contain("frames")) {
        for (const auto& fv : _p.value("frames").to<alx::varvec>()) {
            if (!fv.is<alx::varmap>()) continue;
            const alx::varmap& fm = fv.to<alx::varmap>();
            proto::stack_frame f;
            f.func = str_field(fm, "func");
            f.source = str_field(fm, "source");
            f.ref = str_field(fm, "ref");
            f.alias = str_field(fm, "alias");
            f.line = static_cast<alx::uint_64>(int_field(fm, "line"));
            f.col = static_cast<alx::uint_64>(int_field(fm, "col"));
            f.ofst = static_cast<alx::uint_64>(int_field(fm, "ofst"));
            f.internal = bool_field(fm, "internal");
            t.frames.push_back(std::move(f));
        }
    }
    if (known > m_known) m_known = known;

    if (!m_announced) {
        m_announced = true;
        emit paused();
    }
    emit threadsChanged(thread_list(), m_stop_id, static_cast<int>(m_known),
                        static_cast<int>(m_threads.size()));
    if (reason == QLatin1String("trap") && thread == trigger) emit trapReported(label, args);
    if (thread == m_focus) apply_focus();
}

void Session::apply_focus() {
    const auto it = m_threads.find(m_focus);
    m_frames = it == m_threads.end() ? std::vector<proto::stack_frame>() : it->frames;
    // the innermost visible frame drives the variables and the editor marker
    const proto::stack_frame* view = nullptr;
    for (const auto& f : m_frames) {
        if (!f.ref.empty()) {
            view = &f;
            break;
        }
    }
    m_snapshot_ref = view ? QString::fromStdString(view->ref) : QStringLiteral("root");
    m_runner->send(proto::make_snapshot_request(m_snapshot_ref.toStdString(), 0, 0, m_focus));
    emit breakpointFrames(frames_list(m_frames));
    const QString file = view ? display_path(view->source) : QString();
    emit breakpointHit(file, view ? static_cast<int>(view->line) : 0,
                       view ? static_cast<int>(view->col) : 0);
    emit focusChanged();
}

QVariantList Session::thread_list() const {
    QVariantList list;
    QList<quint64> ids = m_threads.keys();
    std::sort(ids.begin(), ids.end());
    for (quint64 id : ids) {
        const thread_entry& t = m_threads[id];
        const proto::stack_frame* top = t.frames.empty() ? nullptr : &t.frames.front();
        QVariantMap m;
        m[QStringLiteral("id")] = static_cast<qulonglong>(t.id);
        m[QStringLiteral("parent")] = static_cast<qlonglong>(t.parent);
        m[QStringLiteral("trigger")] = t.trigger;
        m[QStringLiteral("label")] = t.label;
        m[QStringLiteral("trapArgs")] = t.trapArgs;
        m[QStringLiteral("captured")] = true;
        m[QStringLiteral("file")] = top ? display_path(top->source) : QString();
        m[QStringLiteral("row")] = top ? static_cast<qulonglong>(top->line) : 0;
        m[QStringLiteral("col")] = top ? static_cast<qulonglong>(top->col) : 0;
        m[QStringLiteral("ofst")] = top ? static_cast<qulonglong>(top->ofst) : 0;
        list.append(m);
    }
    return list;
}

QVariantList Session::frames_list(const std::vector<proto::stack_frame>& _frames) const {
    QVariantList out;
    for (const auto& f : _frames) {
        QVariantMap item;
        item[QStringLiteral("func")] = QString::fromStdString(f.func);
        item[QStringLiteral("alias")] = QString::fromStdString(f.alias);
        item[QStringLiteral("file")] = display_path(f.source);
        item[QStringLiteral("ref")] = QString::fromStdString(f.ref);
        item[QStringLiteral("line")] = static_cast<qulonglong>(f.line);
        item[QStringLiteral("col")] = static_cast<qulonglong>(f.col);
        item[QStringLiteral("ofst")] = static_cast<qulonglong>(f.ofst);
        item[QStringLiteral("root")] = f.func.empty();
        item[QStringLiteral("internal")] = f.internal;
        out.append(item);
    }
    return out;
}

void Session::reset_wave() {
    m_threads.clear();
    m_frames.clear();
    m_snapshot_ref.clear();
    m_stop_id = 0;
    m_focus = 0;
    m_known = 1;
    m_resumed = true;
    m_announced = false;
}

void Session::on_snapshot(const alx::varmap& _p) {
    if (static_cast<quint64>(int_field(_p, "thread")) != m_focus) return;
    const QString ref = QString::fromStdString(str_field(_p, "ref"));
    const quint64 ofst = static_cast<quint64>(int_field(_p, "ofst"));
    QVariantList list;
    if (_p.contain("nodes")) {
        for (const auto& nv : _p.value("nodes").to<alx::varvec>()) {
            if (!nv.is<alx::varmap>()) continue;
            const alx::varmap& n = nv.to<alx::varmap>();
            QVariantMap node;
            node[QStringLiteral("name")] = QString::fromStdString(str_field(n, "name"));
            node[QStringLiteral("type")] = QString::fromStdString(str_field(n, "type"));
            node[QStringLiteral("value")] = QString::fromStdString(str_field(n, "value"));
            node[QStringLiteral("kind")] = QString::fromStdString(str_field(n, "kind"));
            node[QStringLiteral("ref")] = QString::fromStdString(str_field(n, "ref"));
            node[QStringLiteral("handle")] = QString::fromStdString(str_field(n, "handle"));
            node[QStringLiteral("shared")] = bool_field(n, "shared");
            node[QStringLiteral("hasChildren")] = bool_field(n, "hasChildren");
            list.append(node);
        }
    }
    emit snapshotReady(ref, ofst, list, bool_field(_p, "more"));
}

void Session::on_view(const alx::varmap& _p, alx::int_64 _id) {
    if (_id != m_view_seq) return; // stale answer
    if (static_cast<quint64>(int_field(_p, "thread")) != m_focus) return;
    const QString ref = QString::fromStdString(str_field(_p, "ref"));
    const bool ok = bool_field(_p, "ok");
    const QString error = QString::fromStdString(str_field(_p, "error"));
    QVariantMap view;
    if (_p.contain("view")) view = to_qvariant(_p.value("view")).toMap();
    emit viewReady(ref, ok, error, view);
}

void Session::on_exited(int _code, bool _signaled, int _signal, const QString& _stderrTail) {
    if (m_exited_seen) return; // the protocol already reported this run
    if (m_console && m_console_closing) {
        finish(true, QStringLiteral("控制台已关闭"));
        return;
    }
    const QString reason = m_runner ? m_runner->killReason() : QString();
    if (!reason.isEmpty()) {
        finish(false, reason);
        return;
    }
    QString message;
    if (_signaled)
        message = QStringLiteral("runner 崩溃（信号 %1）").arg(_signal);
    else
        message = QStringLiteral("runner 异常退出（code %1）").arg(_code);
    if (!_stderrTail.trimmed().isEmpty()) message += QStringLiteral("\n") + _stderrTail;
    finish(false, message);
}

void Session::finish(bool _ok, const QString& _message) {
    if (m_finished) return;
    m_finished = true;
    m_state = state::finished;
    m_console = false;
    if (!m_logPath.isEmpty()) {
        const QFileInfo fi(m_logPath);
        if (!fi.exists() || fi.size() == 0) {
            m_logPath.clear();
            emit logPathChanged(m_logPath);
            emit logMessage(QStringLiteral("无打印输出，未生成会话日志"));
        }
    }
    emit finished(_ok, _message);
}

void Session::clear_temp() {
    if (!m_has_temp) return;
    m_has_temp = false;
    proto::breakpoint_list items;
    for (const auto& b : m_bps)
        if (b.source == m_temp.source) items.push_back(b);
    m_runner->send(proto::make_set_breakpoints(m_temp.source, items));
}

QString Session::fresh_log() {
    if (!m_run_options.log_file) return QString();
    const QFileInfo dir(QDir::tempPath());
    if (!dir.isDir() || !dir.isWritable()) {
        emit logMessage(QStringLiteral("无法创建会话日志"));
        return QString();
    }
    return QDir::temp().filePath(
        QStringLiteral("axc-session-%1.log").arg(QDateTime::currentMSecsSinceEpoch()));
}

void Session::clear_log_path() {
    if (m_logPath.isEmpty()) return;
    m_logPath.clear();
    emit logPathChanged(m_logPath);
}
