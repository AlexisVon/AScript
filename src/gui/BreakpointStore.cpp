#include "BreakpointStore.h"
#include "QtCompat.h"

#include <QFileInfo>
#include <QSettings>

#include <algorithm>

namespace {
    // key_of — canonical absolute path; empty for an untitled document
    QString canonical(const QString& _path) {
        if (_path.isEmpty()) return QString();
        const QFileInfo fi(_path);
        const QString canon = fi.canonicalFilePath();
        return canon.isEmpty() ? fi.absoluteFilePath() : canon;
    }

    // decode_rows — "line=base64(text),..." into a row map
    QMap<int, QString> decode_rows(const QString& _raw) {
        QMap<int, QString> rows;
        for (const QString& part : _raw.split(QLatin1Char(','), AXC_STRING_SPLIT_SKIP_EMPTY_PARTS)) {
            const int eq = part.indexOf(QLatin1Char('='));
            if (eq <= 0) continue;
            rows.insert(part.left(eq).toInt(),
                        QString::fromUtf8(QByteArray::fromBase64(part.mid(eq + 1).toLatin1(),
                                                                 QByteArray::Base64UrlEncoding)));
        }
        return rows;
    }

    // encode_rows — the inverse of decode_rows
    QString encode_rows(const QMap<int, QString>& _rows) {
        QStringList parts;
        for (auto it = _rows.constBegin(); it != _rows.constEnd(); ++it)
            parts << QString::number(it.key()) + QLatin1Char('=') +
                         QString::fromLatin1(it.value().toUtf8().toBase64(
                             QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
        return parts.join(QLatin1Char(','));
    }

    QSettings& settings() {
        static QSettings s(QStringLiteral("AScript"), QStringLiteral("AScript"));
        return s;
    }

    // settings_key — QSettings treats '/' as a group separator, so encode the path
    QString settings_key(const QString& _path) {
        return QString::fromLatin1(
            _path.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    }

    // settings_path — decode a stored key back into a path
    QString settings_path(const QString& _key) {
        return QString::fromUtf8(QByteArray::fromBase64(_key.toLatin1(), QByteArray::Base64UrlEncoding));
    }
} // namespace

BreakpointStore& BreakpointStore::instance() {
    static BreakpointStore store;
    return store;
}

QSet<int> BreakpointStore::lines(const QString& _path) const {
    return m_lines.value(key_of(_path));
}

bool BreakpointStore::toggle(const QString& _path, int _line) {
    const QString key = key_of(_path);
    if (key.isEmpty() || _line <= 0) return false;
    QSet<int>& rows = m_lines[key];
    const bool on = !rows.contains(_line);
    if (on)
        rows.insert(_line);
    else
        rows.remove(_line);
    if (rows.isEmpty()) m_lines.remove(key);
    save();
    return on;
}

void BreakpointStore::setLog(const QString& _path, int _line, const QString& _log) {
    const QString key = key_of(_path);
    if (key.isEmpty() || _line <= 0) return;
    if (_log.isEmpty()) {
        m_logs[key].remove(_line);
        if (m_logs[key].isEmpty()) m_logs.remove(key);
    } else {
        m_lines[key].insert(_line);
        m_logs[key].insert(_line, _log);
    }
    save();
}

void BreakpointStore::setCondition(const QString& _path, int _line, const QString& _condition) {
    const QString key = key_of(_path);
    if (key.isEmpty() || _line <= 0) return;
    if (_condition.isEmpty()) {
        m_conditions[key].remove(_line);
        if (m_conditions[key].isEmpty()) m_conditions.remove(key);
    } else {
        m_lines[key].insert(_line);
        m_conditions[key].insert(_line, _condition);
    }
    save();
}

QString BreakpointStore::condition(const QString& _path, int _line) const {
    return m_conditions.value(key_of(_path)).value(_line);
}

QString BreakpointStore::log(const QString& _path, int _line) const {
    return m_logs.value(key_of(_path)).value(_line);
}

bool BreakpointStore::hasAny() const {
    for (auto it = m_lines.constBegin(); it != m_lines.constEnd(); ++it)
        if (!it.value().isEmpty()) return true;
    return false;
}

proto::breakpoint_list BreakpointStore::to_list() const {
    proto::breakpoint_list out;
    for (auto it = m_lines.constBegin(); it != m_lines.constEnd(); ++it) {
        QList<int> rows = it.value().values();
        std::sort(rows.begin(), rows.end());
        for (int row : rows)
            out.push_back({"file:" + it.key().toStdString(), static_cast<alx::uint_64>(row), true,
                           m_logs.value(it.key()).value(row).toStdString(),
                           m_conditions.value(it.key()).value(row).toStdString()});
    }
    return out;
}

BreakpointStore::BreakpointStore() {
    settings().beginGroup(QStringLiteral("breakpoints"));
    const QStringList keys = settings().childKeys();
    for (const QString& key : keys) {
        const QStringList parts =
            settings().value(key).toString().split(QLatin1Char(','), AXC_STRING_SPLIT_SKIP_EMPTY_PARTS);
        QSet<int> rows;
        for (const QString& p : parts) rows.insert(p.toInt());
        if (!rows.isEmpty()) m_lines.insert(settings_path(key), rows);
    }
    settings().endGroup();

    for (const char* group : {"breakpointLogs", "breakpointConditions"}) {
        settings().beginGroup(QLatin1String(group));
        for (const QString& key : settings().childKeys()) {
            const QMap<int, QString> rows = decode_rows(settings().value(key).toString());
            if (rows.isEmpty()) continue;
            auto& target = QString(QLatin1String(group)) == QLatin1String("breakpointLogs") ? m_logs : m_conditions;
            target.insert(settings_path(key), rows);
        }
        settings().endGroup();
    }
}

QString BreakpointStore::key_of(const QString& _path) {
    return canonical(_path);
}

void BreakpointStore::save() const {
    QSettings& s = settings();
    s.remove(QStringLiteral("breakpoints"));
    s.beginGroup(QStringLiteral("breakpoints"));
    for (auto it = m_lines.constBegin(); it != m_lines.constEnd(); ++it) {
        QList<int> rows = it.value().values();
        std::sort(rows.begin(), rows.end());
        QStringList parts;
        for (int row : rows) parts << QString::number(row);
        s.setValue(settings_key(it.key()), parts.join(QLatin1Char(',')));
    }
    s.endGroup();

    for (const char* group : {"breakpointLogs", "breakpointConditions"}) {
        const auto& rows =
            QString(QLatin1String(group)) == QLatin1String("breakpointLogs") ? m_logs : m_conditions;
        s.remove(QLatin1String(group));
        s.beginGroup(QLatin1String(group));
        for (auto path = rows.constBegin(); path != rows.constEnd(); ++path)
            s.setValue(settings_key(path.key()), encode_rows(path.value()));
        s.endGroup();
    }
}
