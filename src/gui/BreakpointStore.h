#ifndef ABREAKPOINTSTORE_H
#define ABREAKPOINTSTORE_H

#include "proto/message.h"

#include <QMap>
#include <QSet>
#include <QString>

// BreakpointStore — file → lines, persisted in QSettings under breakpoints/<path>
class BreakpointStore {
public:
    static BreakpointStore& instance();

    // lines — 1-based rows of _path (empty for an untitled document)
    QSet<int> lines(const QString& _path) const;
    // toggle — add or remove one row; true when it is now set
    bool toggle(const QString& _path, int _line);
    // setLog — logpoint text for a row (also enables the row); empty removes only the text
    void setLog(const QString& _path, int _line, const QString& _log);
    // setCondition — host-side predicate for a row (also enables it); empty removes it
    void setCondition(const QString& _path, int _line, const QString& _condition);
    QString condition(const QString& _path, int _line) const;
    QString log(const QString& _path, int _line) const;
    bool hasAny() const;
    // to_list — wire form; source ids match the runner's "file:<canonical path>"
    proto::breakpoint_list to_list() const;

private:
    BreakpointStore();
    static QString key_of(const QString& _path);
    void save() const;

    QMap<QString, QSet<int>> m_lines; // canonical path → rows
    QMap<QString, QMap<int, QString>> m_logs; // canonical path → row → logpoint text
    QMap<QString, QMap<int, QString>> m_conditions; // canonical path → row → predicate
};

#endif // ABREAKPOINTSTORE_H
