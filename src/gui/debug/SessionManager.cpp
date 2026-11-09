#include "SessionManager.h"

SessionManager::SessionManager(QObject* _parent)
    : QObject(_parent) {}

int SessionManager::start(const QString& _code, const QString& _filePath, const QString& _cwd, bool _debug) {
    if (count() >= m_max) return -1;
    auto* s = new Session(this);
    s->setRunnerPath(m_runner);
    const int id = m_next++;
    m_sessions.insert(id, s);
    connect(s, &Session::finished, this, [this, id] { emit finished(id); });
    if (!s->run(_code, _filePath, _cwd, _debug)) {
        m_sessions.remove(id);
        s->deleteLater();
        return -1;
    }
    emit started(id);
    if (!m_active) setActive(id);
    return id;
}

void SessionManager::remove(int _id) {
    Session* s = m_sessions.take(_id);
    if (!s) return;
    s->stop();
    s->deleteLater();
    if (m_active == _id) setActive(m_sessions.isEmpty() ? 0 : m_sessions.firstKey());
}

void SessionManager::closeAll() {
    const QList<int> all = m_sessions.keys();
    for (int id : all) remove(id);
}

Session* SessionManager::at(int _id) const {
    return m_sessions.value(_id, nullptr);
}

Session* SessionManager::active() const {
    return at(m_active);
}

void SessionManager::setActive(int _id) {
    if (_id != 0 && !m_sessions.contains(_id)) return;
    if (m_active == _id) return;
    m_active = _id;
    emit activeChanged(m_active);
}

QList<int> SessionManager::ids() const {
    return m_sessions.keys();
}
