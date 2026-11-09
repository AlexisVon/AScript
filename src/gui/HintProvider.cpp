#include "HintProvider.h"

#include "core/project.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>

#include <algorithm>
#include <utility>

namespace {

    QString linkSuffix() {
#if defined(_WIN32)
        return QStringLiteral(".dll");
#elif defined(__APPLE__)
        return QStringLiteral(".dylib");
#else
        return QStringLiteral(".so");
#endif
    }

    QString cleanDesc(const QString& _s) {
        QString s = _s.trimmed();
        s.replace('\n', ' ').replace('\r', ' ');
        if (s.size() > 80) s = s.left(77) + "...";
        return s;
    }

    // splitTopLevel — split on _sep outside strings and (), [], {}
    QStringList splitTopLevel(const QString& _s, QChar _sep) {
        QStringList out;
        QString cur;
        int depth = 0;
        bool str = false, esc = false;
        QChar quote;
        for (const QChar c : _s) {
            if (str) {
                cur += c;
                if (esc) esc = false;
                else if (c == '\\') esc = true;
                else if (c == quote) str = false;
                continue;
            }
            if (c == '"' || c == '\'' || c == '`') {
                str = true;
                quote = c;
                cur += c;
                continue;
            }
            if (c == '(' || c == '[' || c == '{') ++depth;
            else if (c == ')' || c == ']' || c == '}') --depth;
            if (c == _sep && depth == 0) {
                out << cur;
                cur.clear();
                continue;
            }
            cur += c;
        }
        out << cur;
        return out;
    }

    QStringList paramNames(const QString& _params) {
        static const QRegularExpression id(QStringLiteral("^[A-Za-z_]\\w*$"));
        QStringList out;
        for (QString p : splitTopLevel(_params, ',')) {
            p = p.trimmed();
            const int eq = p.indexOf('=');
            if (eq >= 0) p = p.left(eq).trimmed();
            if (id.match(p).hasMatch()) out << p;
        }
        return out;
    }

    // stripComments — drop // and /* */ while keeping string literals intact
    QString stripComments(const QString& _line, bool& _block) {
        QString out;
        bool str = false, esc = false;
        QChar quote;
        for (int i = 0; i < _line.size(); ++i) {
            const QChar c = _line[i];
            if (_block) {
                if (c == '*' && i + 1 < _line.size() && _line[i + 1] == '/') {
                    _block = false;
                    ++i;
                }
                continue;
            }
            if (str) {
                out += c;
                if (esc) esc = false;
                else if (c == '\\') esc = true;
                else if (c == quote) str = false;
                continue;
            }
            if (c == '"' || c == '\'' || c == '`') {
                str = true;
                quote = c;
                out += c;
                continue;
            }
            if (c == '/' && i + 1 < _line.size()) {
                if (_line[i + 1] == '/') break;
                if (_line[i + 1] == '*') {
                    _block = true;
                    ++i;
                    continue;
                }
            }
            out += c;
        }
        return out;
    }

    int braceDelta(const QString& _line) {
        int delta = 0;
        bool str = false, esc = false;
        QChar quote;
        for (const QChar c : _line) {
            if (str) {
                if (esc) esc = false;
                else if (c == '\\') esc = true;
                else if (c == quote) str = false;
                continue;
            }
            if (c == '"' || c == '\'' || c == '`') {
                str = true;
                quote = c;
                continue;
            }
            if (c == '{') ++delta;
            else if (c == '}') --delta;
        }
        return delta;
    }

    // link docs are named after the target stem: cv.so / cv -> cv.json
    QString jsonPathForLink(const QString& _path) {
        const QFileInfo fi(_path);
        QString base = fi.fileName();
        for (const QString& suffix : {QStringLiteral(".so"), QStringLiteral(".dll"), QStringLiteral(".dylib")})
            if (base.endsWith(suffix)) {
                base.chop(suffix.size());
                break;
            }
        return fi.absolutePath() + QStringLiteral("/") + base + QStringLiteral(".json");
    }

    QString linkSourceName(const QString& _target) {
        const QFileInfo fi(_target);
        const QString base = fi.fileName();
        for (const QString& suffix : {QStringLiteral(".so"), QStringLiteral(".dll"), QStringLiteral(".dylib")})
            if (base.endsWith(suffix)) return base;
        if (!fi.suffix().isEmpty()) return base;
        return base + linkSuffix();
    }

} // namespace

void HintProvider::setProject(const QString& _projectDir, const QStringList& _path,
                              const QStringList& _docpath, const QStringList& _userDirs) {
    m_projectDir = _projectDir;
    m_path = _path;
    m_docpath = _docpath;
    m_userDirs = _userDirs;
}

namespace {
    QString expandLibraryPath(const QString& _expr, const QString& _base) {
        return QString::fromStdString(
            core::project::expand_path(_expr.toStdString(), _base.toStdString()));
    }
} // namespace

void HintProvider::setLimit(int _entries) {
    m_limit = qMax(0, _entries);
    evict();
}

void HintProvider::clear() {
    for (auto& pool : m_pools) pool.clear();
    m_lru.clear();
    m_total = 0;
    m_links.clear();
    m_imports.clear();
    m_aliasHash = 0;
    m_aliasLength = -1;
}

void HintProvider::reload(const QList<QPair<QString, QString>>& _docs) {
    clear();
    for (const auto& doc : _docs) {
        scanAliases(doc.second);
        const QString dir = moduleDirOf(doc.first);
        for (auto it = m_links.constBegin(); it != m_links.constEnd(); ++it)
            cached(Kind::Link, it->path, dir);
        for (auto it = m_imports.constBegin(); it != m_imports.constEnd(); ++it)
            cached(Kind::Import, it->path, dir);
    }
}

QList<CompletionItemData> HintProvider::complete(const QString& _text, const QString& _filePath,
                                                 const QString& _chain, const QString& _partial) {
    scanAliases(_text);
    const QString alias = _chain.section('.', 0, 0);
    Kind kind;
    QString target;
    const auto linkIt = m_links.constFind(alias);
    if (linkIt != m_links.constEnd()) {
        kind = Kind::Link;
        target = linkIt->path;
    } else {
        const auto importIt = m_imports.constFind(alias);
        if (importIt == m_imports.constEnd()) return {};
        kind = Kind::Import;
        target = importIt->path;
    }

    const Doc* doc = cached(kind, target, moduleDirOf(_filePath));
    if (!doc) return {};

    // a second segment selects an area; "*" covers runtime instances
    const QList<Symbol>* symbols = &doc->symbols;
    if (_chain.contains('.')) {
        const auto areaIt = doc->areas.constFind(_chain.section('.', 1, 1));
        if (areaIt != doc->areas.constEnd()) symbols = &areaIt.value();
        else if (!doc->wildcard.isEmpty()) symbols = &doc->wildcard;
        else return {}; // no areas: alias.<anything>. has nothing to offer
    }

    QList<CompletionItemData> out;
    for (const Symbol& s : *symbols) {
        if (!_partial.isEmpty() && !s.name.startsWith(_partial, Qt::CaseInsensitive)) continue;
        CompletionItemData d;
        d.name = s.name;
        d.sig = s.sig;
        d.desc = s.desc;
        d.kind = kind == Kind::Link ? CompletionItemData::Link : CompletionItemData::Import;
        d.params = s.params;
        d.source = doc->source;
        d.call = s.call;
        out.append(d);
    }
    return out;
}

QList<HintProvider::LibraryFile> HintProvider::libraryFiles() const {
    QStringList roots;
    auto add = [&](const QString& _d) {
        if (!_d.isEmpty() && !roots.contains(_d)) roots << _d;
    };
    const QString base = m_projectDir.isEmpty() ? QDir::currentPath() : m_projectDir;
    for (const QString& p : m_userDirs) add(expandLibraryPath(p, base));
    for (const QString& p : m_path) add(p);
    for (const QString& p : m_docpath) add(p);
    if (!m_projectDir.isEmpty()) {
        add(m_projectDir + QStringLiteral("/link"));
        add(m_projectDir + QStringLiteral("/import"));
    }
    for (const auto& p : core::project::system_library_paths())
        add(QString::fromStdString(p));

    QList<LibraryFile> out;
    for (const QString& dir : roots) {
        QDir d(dir);
        if (!d.exists()) continue;
        const QFileInfoList links = d.entryInfoList(
            {QStringLiteral("*.so"), QStringLiteral("*.dll"), QStringLiteral("*.dylib")},
            QDir::Files, QDir::Name);
        for (const QFileInfo& fi : links) {
            if (!QFileInfo(jsonPathForLink(fi.absoluteFilePath())).isFile()) continue;
            out.append({dir, fi.absoluteFilePath(), fi.fileName(), Kind::Link});
        }
        const QFileInfoList products =
            d.entryInfoList({QStringLiteral("*.axp")}, QDir::Files, QDir::Name);
        for (const QFileInfo& fi : products)
            out.append({dir, fi.absoluteFilePath(), fi.fileName(), Kind::Import});
    }
    return out;
}

QString HintProvider::documentHtml(Kind _kind, const QString& _absolutePath) {
    const QFileInfo fi(_absolutePath);
    const Doc* doc = cached(_kind, fi.absoluteFilePath(), fi.absolutePath());
    if (!doc) {
        if (_kind == Kind::Import && fi.suffix() == QLatin1String("axp")) {
            core::project::axp_view view;
            std::string err;
            if (core::project::unpack_axp(fi.absoluteFilePath().toStdString(), view, err) &&
                !view.has_hint)
                return QStringLiteral("<h2 style=\"color:#fbf1c7\">%1</h2>"
                                      "<p style=\"color:#a89984\">该 axp 没有 hint 文档</p>")
                    .arg(fi.fileName().toHtmlEscaped());
        }
        return {};
    }
    auto esc = [](const QString& _s) { return _s.toHtmlEscaped(); };
    auto sym = [&](const Symbol& _s) {
        QString row =
            QStringLiteral("<p style=\"margin:2px 0\"><span style=\"color:#83a598;font-family:'Fira Code'\">") +
            esc(_s.name) + esc(_s.sig) + QStringLiteral("</span>");
        if (!_s.desc.isEmpty())
            row += QStringLiteral("<br><span style=\"color:#a89984\">") + esc(_s.desc) +
                   QStringLiteral("</span>");
        return row + QStringLiteral("</p>");
    };
    QString html = QStringLiteral("<h2 style=\"color:#fbf1c7\">") + esc(fi.fileName()) +
                   QStringLiteral("</h2><p style=\"color:#928374\">") + esc(_absolutePath) +
                   QStringLiteral("</p>");
    if (doc->symbols.isEmpty()) html += QStringLiteral("<p style=\"color:#a89984\">（无条目）</p>");
    for (const Symbol& s : doc->symbols) html += sym(s);
    for (auto it = doc->areas.begin(); it != doc->areas.end(); ++it) {
        html += QStringLiteral("<h3 style=\"color:#fbf1c7\">area: ") + esc(it.key()) +
                QStringLiteral("</h3>");
        for (const Symbol& s : it.value()) html += sym(s);
    }
    if (!doc->wildcard.isEmpty()) {
        html += QStringLiteral("<h3 style=\"color:#fbf1c7\">area: *</h3>");
        for (const Symbol& s : doc->wildcard) html += sym(s);
    }
    return html;
}

HintProvider::Doc* HintProvider::cached(Kind _kind, const QString& _target, const QString& _moduleDir) {
    const bool absolute = QFileInfo(_target).isAbsolute();
    const QString key = cacheKey(absolute, _kind, _target, _moduleDir);

    if (absolute) {
        const int pool = poolIndex(_kind, Source::Absolute);
        auto it = m_pools[pool].find(key);
        if (it != m_pools[pool].end()) {
            touch(pool, key);
            return &it.value();
        }
    } else {
        for (const Source src : {Source::System, Source::User}) {
            const int pool = poolIndex(_kind, src);
            auto it = m_pools[pool].find(key);
            if (it != m_pools[pool].end()) {
                touch(pool, key);
                return &it.value();
            }
        }
    }

    Source src;
    const QString resolved = resolve(_kind, _target, _moduleDir, src);
    if (resolved.isEmpty()) return nullptr;

    const QString source = _kind == Kind::Link ? linkSourceName(_target) : QFileInfo(resolved).fileName();
    Doc doc;
    if (!load(_kind, resolved, source, doc)) return nullptr;
    insert(_kind, src, key, std::move(doc));

    const int pool = poolIndex(_kind, src);
    auto it = m_pools[pool].find(key);
    return it != m_pools[pool].end() ? &it.value() : nullptr;
}

bool HintProvider::load(Kind _kind, const QString& _docPath, const QString& _source, Doc& _doc) {
    _doc = Doc{};
    _doc.source = _source;

    if (_kind == Kind::Link) {
        QFile f(_docPath);
        if (!f.open(QIODevice::ReadOnly)) return false;
        QJsonParseError err;
        const QJsonDocument json = QJsonDocument::fromJson(f.readAll(), &err);
        if (err.error != QJsonParseError::NoError || !json.isObject()) return false;
        parseFunctions(json.object(), _doc.symbols);
        parseAreas(json.object(), _doc);
    } else {
        core::project::axp_view view;
        std::string err;
        if (core::project::unpack_axp(_docPath.toStdString(), view, err)) {
            if (!view.has_hint) return false;
            QJsonParseError jerr;
            const QJsonDocument json =
                QJsonDocument::fromJson(QByteArray::fromStdString(view.hint_text), &jerr);
            if (jerr.error != QJsonParseError::NoError || !json.isObject()) return false;
            parseFunctions(json.object(), _doc.symbols);
            parseVariables(json.object(), _doc.symbols);
            parseAreas(json.object(), _doc);
        } else {
            if (QFileInfo(_docPath).suffix() == QLatin1String("axp")) return false; // corrupt product
            QFile f(_docPath);
            if (!f.open(QIODevice::ReadOnly)) return false;
            scanSource(QString::fromUtf8(f.readAll()), _doc.symbols);
        }
    }

    auto sort = [](QList<Symbol>& _symbols) {
        std::stable_sort(_symbols.begin(), _symbols.end(),
                         [](const Symbol& _a, const Symbol& _b) { return _a.name < _b.name; });
    };
    sort(_doc.symbols);
    for (auto it = _doc.areas.begin(); it != _doc.areas.end(); ++it) sort(it.value());
    sort(_doc.wildcard);
    return true;
}

QString HintProvider::resolve(Kind _kind, const QString& _target, const QString& _moduleDir, Source& _src) {
    auto hit = [&](const QString& _candidate, Source _sr) -> QString {
        const QFileInfo fi(_candidate);
        if (!fi.isFile()) return {};
        _src = _sr;
        const QString canonical = fi.canonicalFilePath();
        return canonical.isEmpty() ? fi.absoluteFilePath() : canonical;
    };

    struct Root {
        QString dir;
        Source src;
    };
    QList<Root> roots;
    roots.append(Root{_moduleDir, Source::System});
    for (const QString& p : m_path) roots.append(Root{expandLibraryPath(p, _moduleDir), Source::User});
    roots.append(Root{QDir::currentPath(), Source::System});
    for (const QString& p : m_docpath) roots.append(Root{expandLibraryPath(p, _moduleDir), Source::User});
    if (!m_projectDir.isEmpty()) {
        const QString def = m_projectDir + (_kind == Kind::Link ? QStringLiteral("/link") : QStringLiteral("/import"));
        roots.append(Root{def, Source::System});
    }
    for (const QString& p : m_userDirs) roots.append(Root{expandLibraryPath(p, _moduleDir), Source::User});
    for (const auto& p : core::project::system_library_paths())
        roots.append(Root{QString::fromStdString(p), Source::System});

    // link hints resolve the .json, never requiring the .so to exist
    if (_kind == Kind::Link) {
        if (QFileInfo(_target).isAbsolute()) return hit(jsonPathForLink(_target), Source::Absolute);
        for (const Root& root : roots) {
            if (root.dir.isEmpty()) continue;
            const QString resolved = hit(jsonPathForLink(QDir(root.dir).filePath(_target)), root.src);
            if (!resolved.isEmpty()) return resolved;
        }
        return {};
    }

    const QStringList suffixes{QStringLiteral(".axp"), QStringLiteral(".axc")};
    const bool hasSuffix = !QFileInfo(_target).suffix().isEmpty();

    if (QFileInfo(_target).isAbsolute()) {
        QString resolved = hit(_target, Source::Absolute);
        if (!resolved.isEmpty()) return resolved;
        if (!hasSuffix) {
            for (const QString& suffix : suffixes) {
                resolved = hit(_target + suffix, Source::Absolute);
                if (!resolved.isEmpty()) return resolved;
            }
        }
        return {};
    }

    auto lookup = [&](const QString& _rel) -> QString {
        for (const Root& root : roots) {
            if (root.dir.isEmpty()) continue;
            const QString resolved = hit(QDir(root.dir).filePath(_rel), root.src);
            if (!resolved.isEmpty()) return resolved;
        }
        return {};
    };

    QString resolved = lookup(_target);
    if (!resolved.isEmpty()) return resolved;
    if (!hasSuffix) {
        for (const QString& suffix : suffixes) {
            resolved = lookup(_target + suffix);
            if (!resolved.isEmpty()) return resolved;
        }
    }
    return {};
}

void HintProvider::insert(Kind _kind, Source _src, const QString& _key, Doc&& _doc) {
    const int pool = poolIndex(_kind, _src);
    auto it = m_pools[pool].find(_key);
    if (it != m_pools[pool].end()) {
        it.value() = std::move(_doc);
        touch(pool, _key);
        return;
    }
    m_pools[pool].insert(_key, std::move(_doc));
    ++m_total;
    m_lru.append({pool, _key});
    evict();
}

void HintProvider::touch(int _pool, const QString& _key) {
    for (int i = 0; i < m_lru.size(); ++i) {
        if (m_lru[i].first == _pool && m_lru[i].second == _key) {
            m_lru.removeAt(i);
            break;
        }
    }
    m_lru.append({_pool, _key});
}

void HintProvider::evict() {
    if (m_limit <= 0) return;
    while (m_total > m_limit && !m_lru.isEmpty()) {
        const QPair<int, QString> oldest = m_lru.takeFirst();
        if (m_pools[oldest.first].remove(oldest.second) > 0) --m_total;
    }
}

void HintProvider::scanAliases(const QString& _text) {
    const quint64 hash = static_cast<quint64>(qHash(_text));
    if (hash == m_aliasHash && _text.size() == m_aliasLength) return;
    m_aliasHash = hash;
    m_aliasLength = _text.size();
    m_links.clear();
    m_imports.clear();

    static const QRegularExpression linkRe(QStringLiteral("^\\s*link\\s+\"([^\"]+)\"\\s+as\\s+([A-Za-z_]\\w*)\\s*;"));
    static const QRegularExpression importRe(QStringLiteral("^\\s*import\\s+\"([^\"]+)\"\\s+as\\s+([A-Za-z_]\\w*)\\s*;"));

    bool block = false;
    for (const QString& raw : _text.split('\n')) {
        const QString line = stripComments(raw, block);
        QRegularExpressionMatch m = linkRe.match(line);
        if (m.hasMatch()) {
            m_links.insert(m.captured(2), Target{m.captured(1), Kind::Link});
            continue;
        }
        m = importRe.match(line);
        if (m.hasMatch()) m_imports.insert(m.captured(2), Target{m.captured(1), Kind::Import});
    }
}

QString HintProvider::moduleDirOf(const QString& _filePath) const {
    if (!_filePath.isEmpty()) {
        const QString dir = QFileInfo(_filePath).absolutePath();
        if (!dir.isEmpty()) return dir;
    }
    return m_projectDir.isEmpty() ? QDir::currentPath() : m_projectDir;
}

QString HintProvider::cacheKey(bool _absolute, Kind _kind, const QString& _target, const QString& _moduleDir) const {
    const QChar k = _kind == Kind::Link ? QLatin1Char('l') : QLatin1Char('i');
    if (_absolute) return QStringLiteral("A|") + k + QLatin1Char('|') + QFileInfo(_target).absoluteFilePath();
    return QStringLiteral("R|") + k + QLatin1Char('|') + _target + QLatin1Char('|') + _moduleDir +
           QLatin1Char('|') + m_projectDir;
}

void HintProvider::parseFunctions(const QJsonObject& _root, QList<Symbol>& _out) {
    const QJsonObject funcs = _root.value(QStringLiteral("functions")).toObject();
    for (auto it = funcs.begin(); it != funcs.end(); ++it) {
        const QJsonArray overloads = it.value().toArray();
        if (overloads.isEmpty()) {
            Symbol s;
            s.name = it.key();
            s.sig = QStringLiteral("()");
            _out.append(s);
            continue;
        }
        for (const QJsonValue& value : overloads) {
            const QJsonObject o = value.toObject();
            QStringList sigParams, params;
            for (const QJsonValue& pv : o.value(QStringLiteral("param")).toArray()) {
                const QJsonObject p = pv.toObject();
                const QString key = p.value(QStringLiteral("key")).toString();
                const QString type = p.value(QStringLiteral("type")).toString();
                QString part = key.endsWith(QStringLiteral("..."))
                                   ? key
                                   : (type.isEmpty() ? key : key + QStringLiteral(": ") + type);
                if (p.value(QStringLiteral("optional")).toBool()) part = QLatin1Char('[') + part + QLatin1Char(']');
                sigParams.append(part);
                params.append(key.endsWith(QStringLiteral("...")) ? key.left(key.size() - 3) : key);
            }
            QString sig = QLatin1Char('(') + sigParams.join(QStringLiteral(", ")) + QLatin1Char(')');
            const QString ret = o.value(QStringLiteral("ret")).toObject().value(QStringLiteral("type")).toString();
            if (!ret.isEmpty()) sig += QStringLiteral(" → ") + ret;

            Symbol s;
            s.name = it.key();
            s.sig = sig;
            s.desc = cleanDesc(o.value(QStringLiteral("desc")).toString());
            s.params = params;
            _out.append(s);
        }
    }
}

void HintProvider::parseVariables(const QJsonObject& _root, QList<Symbol>& _out) {
    const QJsonObject vars = _root.value(QStringLiteral("variables")).toObject();
    for (auto it = vars.begin(); it != vars.end(); ++it) {
        Symbol s;
        s.name = it.key();
        s.desc = cleanDesc(it.value().toObject().value(QStringLiteral("desc")).toString());
        s.call = false;
        _out.append(s);
    }
}

void HintProvider::parseAreas(const QJsonObject& _root, Doc& _doc) {
    const QJsonObject areas = _root.value(QStringLiteral("areas")).toObject();
    for (auto it = areas.begin(); it != areas.end(); ++it) {
        const QJsonObject area = it.value().toObject();
        if (area.isEmpty()) continue;
        QList<Symbol> symbols;
        parseFunctions(area, symbols);
        parseVariables(area, symbols);
        if (it.key() == QLatin1String("*")) _doc.wildcard = std::move(symbols);
        else _doc.areas.insert(it.key(), std::move(symbols));
    }
}

void HintProvider::scanSource(const QString& _text, QList<Symbol>& _out) {
    static const QRegularExpression defRe(
        QStringLiteral("^\\s*def\\s+([A-Za-z_]\\w*)\\s*\\((.*)\\)"));
    static const QRegularExpression varRe(QStringLiteral("^\\s*var\\s+(.+)$"));
    static const QRegularExpression idRe(QStringLiteral("^[A-Za-z_]\\w*$"));

    bool block = false;
    int depth = 0;
    for (const QString& raw : _text.split('\n')) {
        const QString line = stripComments(raw, block);
        if (depth == 0) {
            const QRegularExpressionMatch dm = defRe.match(line);
            if (dm.hasMatch()) {
                Symbol s;
                s.name = dm.captured(1);
                const QString params = dm.captured(2).trimmed();
                s.sig = QLatin1Char('(') + params + QLatin1Char(')');
                s.params = paramNames(params);
                _out.append(s);
            } else {
                const QRegularExpressionMatch vm = varRe.match(line);
                if (vm.hasMatch()) {
                    QString decl = vm.captured(1);
                    const int semi = decl.indexOf(QLatin1Char(';'));
                    if (semi >= 0) decl = decl.left(semi);
                    for (QString part : splitTopLevel(decl, QLatin1Char(','))) {
                        part = part.trimmed();
                        const int eq = part.indexOf(QLatin1Char('='));
                        if (eq >= 0) part = part.left(eq).trimmed();
                        if (!idRe.match(part).hasMatch()) continue;
                        Symbol s;
                        s.name = part;
                        s.call = false;
                        _out.append(s);
                    }
                }
            }
        }
        depth += braceDelta(line);
        if (depth < 0) depth = 0;
    }
}
