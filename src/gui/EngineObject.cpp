#include "EngineObject.h"

#include "BreakpointStore.h"
#include "Settings.h"
#include "debug/Session.h"

#include <QDir>
#include <QFileInfo>

EngineObject::EngineObject(QObject* _parent)
    : QObject(_parent) {
    m_session = new Session(this);
    m_session->setRunnerPath(Settings::runnerPath());
    connect(m_session, &Session::outputText, this, &EngineObject::outputText);
    connect(m_session, &Session::inputRequested, this, &EngineObject::inputRequested);
    connect(m_session, &Session::execResult, this, &EngineObject::execResult);
    connect(m_session, &Session::finished, this, &EngineObject::finished);
    connect(m_session, &Session::logMessage, this, &EngineObject::logMessage);
    connect(m_session, &Session::runnerStarted, this, &EngineObject::runnerStarted);
    connect(m_session, &Session::paused, this, &EngineObject::paused);
    connect(m_session, &Session::breakpointHit, this, &EngineObject::breakpointHit);
    connect(m_session, &Session::breakpointFrames, this, &EngineObject::breakpointFrames);
    connect(m_session, &Session::snapshotReady, this, &EngineObject::snapshotReady);
    connect(m_session, &Session::viewReady, this, &EngineObject::viewReady);
    connect(m_session, &Session::consoleReady, this, &EngineObject::consoleReady);
    connect(m_session, &Session::logPathChanged, this, &EngineObject::logPathChanged);
    connect(m_session, &Session::threadsChanged, this, &EngineObject::threadsChanged);
    connect(m_session, &Session::focusChanged, this, &EngineObject::focusChanged);
    connect(m_session, &Session::trapReported, this, &EngineObject::trapReported);
}

bool EngineObject::isRunning() const {
    return m_session && m_session->isRunning();
}

EngineObject::~EngineObject() {
    if (m_session) m_session->stop();
}

void EngineObject::prepare_session() {
    m_session->setRunnerPath(Settings::runnerPath());
    m_session->set_limits(Settings::runWallMs(), Settings::runMemBytes());
    m_session->set_log_keep_days(Settings::logKeepDays());
    RunnerProcess::run_options opts;
    opts.frame_ms = Settings::runFrameMs();
    opts.frame_bytes = Settings::runFrameBytes();
    opts.trim_bytes = Settings::runTrimBytes();
    opts.log_file = Settings::runLogFile();
    m_session->set_run_options(opts);
    m_session->set_trace_path(m_trace_path);
    m_session->set_multi_thread(Settings::multiThreadDebug());
}

void EngineObject::runText(const QString& _code, const QString& _filePath, bool _debug) {
    if (isRunning()) return;
    if (!Settings::runnerEnabled()) {
        m_session->clear_log_path();
        emit finished(false, QStringLiteral("脚本执行已在设置中禁用"));
        return;
    }
    const bool debug = _debug || Settings::debugOnRun();
    const QString cwd = _filePath.isEmpty() ? QDir::currentPath() : QFileInfo(_filePath).absolutePath();
    prepare_session();
    m_session->set_breakpoints(debug ? BreakpointStore::instance().to_list() : proto::breakpoint_list());
    if (!m_session->run(_code, _filePath, cwd, debug))
        emit finished(false, m_session->lastError());
}

void EngineObject::runAx(const QString& _axPath, bool _debug) {
    if (isRunning()) return;
    if (!Settings::runnerEnabled()) {
        m_session->clear_log_path();
        emit finished(false, QStringLiteral("脚本执行已在设置中禁用"));
        return;
    }
    const bool debug = _debug || Settings::debugOnRun();
    prepare_session();
    m_session->set_breakpoints(debug ? BreakpointStore::instance().to_list() : proto::breakpoint_list());
    if (!m_session->run(QString(), _axPath, QFileInfo(_axPath).absolutePath(), debug))
        emit finished(false, m_session->lastError());
}

void EngineObject::submitInput(const QString& _line) {
    if (m_session && m_session->isRunning()) m_session->submitInput(_line);
}

void EngineObject::continueRun() {
    if (m_session && m_session->isRunning()) m_session->continue_run();
}

void EngineObject::interruptRun() {
    if (m_session && m_session->isRunning()) m_session->stop();
}

void EngineObject::pauseRun() {
    if (m_session && m_session->isRunning()) m_session->pause_run();
}

void EngineObject::runToCursor(const QString& _path, int _row) {
    if (!m_session || !m_session->isPaused()) return;
    m_session->run_to_line(_path, _row);
}

QString EngineObject::logPath() const {
    return m_session ? m_session->logPath() : QString();
}

unsigned EngineObject::runnerPid() const {
    return m_session ? static_cast<unsigned>(m_session->pid()) : 0u;
}

bool EngineObject::openConsole() {
    if (!m_session || m_session->isRunning()) return false;
    prepare_session();
    m_session->set_breakpoints({});
    m_session->set_stop_on_entry(false);
    if (!m_session->start_console(QDir::currentPath())) {
        emit logMessage(m_session->lastError());
        return false;
    }
    return true;
}

void EngineObject::resetConsole() {
    if (m_session) m_session->reset_console();
}

bool EngineObject::consoleExec(const QString& _line) {
    return m_session && m_session->console_exec(_line);
}

void EngineObject::closeConsole() {
    if (m_session) m_session->close_console();
}

bool EngineObject::consoleActive() const {
    return m_session && m_session->is_console();
}

QString EngineObject::snapshotRef() const {
    return m_session ? m_session->snapshot_ref() : QString();
}

void EngineObject::requestSnapshot(const QString& _ref, quint64 _ofst) {
    if (m_session && m_session->isPaused()) m_session->request_snapshot(_ref, _ofst);
}

void EngineObject::requestView(const QString& _ref) {
    if (m_session && m_session->isPaused()) m_session->request_view(_ref);
}

void EngineObject::syncBreakpoints(const QString& _path) {
    if (!m_session) return;
    m_session->set_breakpoints(BreakpointStore::instance().to_list());
    m_session->push_breakpoints(_path);
}

void EngineObject::stepOver() {
    if (m_session && m_session->isPaused()) m_session->step_over();
}

void EngineObject::stepInto() {
    if (m_session && m_session->isPaused()) m_session->step_into();
}

void EngineObject::stepOut() {
    if (m_session && m_session->isPaused()) m_session->step_out();
}

void EngineObject::setSkipTraps(bool _on) {
    m_skip_traps = _on;
    if (m_session) m_session->set_skip_traps(_on);
}

bool EngineObject::skipTraps() const {
    return m_skip_traps;
}

void EngineObject::setMultiThread(bool _on) {
    if (m_session) m_session->set_multi_thread(_on);
}

void EngineObject::setFocusThread(quint64 _id) {
    if (m_session) m_session->set_focus_thread(_id);
}

quint64 EngineObject::focusThread() const {
    return m_session ? m_session->focusThread() : 0;
}
