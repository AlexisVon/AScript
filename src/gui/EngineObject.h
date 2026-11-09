#ifndef ENGINEOBJECT_H
#define ENGINEOBJECT_H

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

class Session;

// EngineObject — main-thread façade over the runner-backed Session
class EngineObject : public QObject {
    Q_OBJECT

public:
    explicit EngineObject(QObject* _parent = nullptr);
    ~EngineObject() override;

    bool isRunning() const;

public slots:
    void runText(const QString& _code, const QString& _filePath, bool _debug);
    void runAx(const QString& _axPath, bool _debug);
    void submitInput(const QString& _line);
    void continueRun();
    void interruptRun();
    void pauseRun();
    void runToCursor(const QString& _path, int _row);
    QString logPath() const; // raw session log of the last run, empty when none
    unsigned runnerPid() const; // 0 when nothing runs
    bool openConsole();
    void resetConsole();
    bool consoleExec(const QString& _line);
    void closeConsole();
    bool consoleActive() const;
    QString snapshotRef() const;
    void requestSnapshot(const QString& _ref, quint64 _ofst = 0);
    void requestView(const QString& _ref);
    void syncBreakpoints(const QString& _path);
    void stepOver();
    void stepInto();
    void stepOut();
    void setSkipTraps(bool _on);
    bool skipTraps() const;
    void setMultiThread(bool _on);
    void setFocusThread(quint64 _id);
    quint64 focusThread() const;
    // setRunnerTracePath — GUI protocol trace file for the next and later runs
    void setRunnerTracePath(const QString& _path) { m_trace_path = _path; }

signals:
    void outputText(const QString& _text);
    void inputRequested();
    void breakpointHit(const QString& _file, int _row, int _col);
    void breakpointFrames(const QVariantList& _frames);
    void paused();
    void execResult(const QString& _value, qint64 _elapsedUs, bool _ok, int _errorType);
    void finished(bool _ok, const QString& _message);
    void logMessage(const QString& _text); // runner diagnostics, not script output
    void runnerStarted(quint64 _pid);
    void consoleReady();
    void snapshotReady(const QString& _ref, quint64 _ofst, const QVariantList& _nodes, bool _more);
    void viewReady(const QString& _ref, bool _ok, const QString& _error, const QVariantMap& _view);
    void logPathChanged(const QString& _path); // session log of the new run; empty when logging is off
    void threadsChanged(const QVariantList& _threads, quint64 _stop, int _known, int _captured);
    void focusChanged();
    void trapReported(const QString& _label, const QString& _args);

private:
    void prepare_session();

    Session* m_session = nullptr;
    bool m_skip_traps = false;
    QString m_trace_path;
};
#endif // ENGINEOBJECT_H
