#ifndef AGUI_SESSIONMANAGER_H
#define AGUI_SESSIONMANAGER_H

#include "Session.h"

#include <QList>
#include <QMap>
#include <QObject>
#include <QString>

// SessionManager — several concurrent sessions, one of them active
class SessionManager : public QObject {
    Q_OBJECT

public:
    explicit SessionManager(QObject* _parent = nullptr);

    void setRunnerPath(const QString& _path) { m_runner = _path; }
    // setMax — refuse new sessions past this count (default 8)
    void setMax(int _max) { m_max = _max; }

    // start — one new session; returns its id, or -1 when refused
    int start(const QString& _code, const QString& _filePath, const QString& _cwd, bool _debug = false);
    void remove(int _id);
    void closeAll();

    Session* at(int _id) const;
    Session* active() const;
    void setActive(int _id);
    int activeId() const { return m_active; }
    QList<int> ids() const;
    int count() const { return static_cast<int>(m_sessions.size()); }

signals:
    void started(int _id);
    void finished(int _id);
    void activeChanged(int _id);

private:
    QMap<int, Session*> m_sessions;
    QString m_runner;
    int m_next = 1;
    int m_active = 0;
    int m_max = 8;
};

#endif // AGUI_SESSIONMANAGER_H
