#ifndef HINTPROVIDER_H
#define HINTPROVIDER_H

#include <QHash>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include "CompletionItem.h"

class QJsonObject;

// HintProvider — link/import completion docs, decoupled from the editor and the
// base completion pool. Six LRU pools: {link,import} × {absolute,user,system}.
class HintProvider {
public:
    enum class Kind { Link,
                      Import };

    // one library artifact found under a configured root
    struct LibraryFile {
        QString dir;
        QString path;
        QString name;
        Kind kind;
    };

    void setProject(const QString& _projectDir, const QStringList& _path, const QStringList& _docpath,
                    const QStringList& _userDirs = {});
    void setLimit(int _entries); // global cap across all pools, 0 = unlimited
    void clear();
    void reload(const QList<QPair<QString, QString>>& _docs); // {filePath, text}
    // _chain is the dotted text before the partial ("cv" / "olm.llm"); the head
    // segment is the alias, an optional second segment selects an area.
    QList<CompletionItemData> complete(const QString& _text, const QString& _filePath,
                                       const QString& _chain, const QString& _partial);
    int size() const { return m_total; }

    // library docs page: roots shared with the hint pools; files are listed lazily
    QList<LibraryFile> libraryFiles() const;
    QString documentHtml(Kind _kind, const QString& _absolutePath);

private:
    enum class Source { Absolute,
                        User,
                        System };
    struct Symbol {
        QString name;
        QString sig;
        QString desc;
        QStringList params;
        bool call = true; // import variables are not callable
    };
    struct Doc {
        QString source; // file name shown as the provenance tag
        QList<Symbol> symbols;
        QHash<QString, QList<Symbol>> areas;
        QList<Symbol> wildcard; // areas["*"] — runtime instances
    };
    struct Target {
        QString path;
        Kind kind;
    };

    Doc* cached(Kind _kind, const QString& _target, const QString& _moduleDir);
    bool load(Kind _kind, const QString& _docPath, const QString& _source, Doc& _doc);
    QString resolve(Kind _kind, const QString& _target, const QString& _moduleDir, Source& _src);
    void insert(Kind _kind, Source _src, const QString& _key, Doc&& _doc);
    void touch(int _pool, const QString& _key);
    void evict();
    void scanAliases(const QString& _text);
    QString moduleDirOf(const QString& _filePath) const;
    QString cacheKey(bool _absolute, Kind _kind, const QString& _target, const QString& _moduleDir) const;
    static int poolIndex(Kind _kind, Source _src) { return static_cast<int>(_kind) * 3 + static_cast<int>(_src); }

    static void parseFunctions(const QJsonObject& _root, QList<Symbol>& _out);
    static void parseVariables(const QJsonObject& _root, QList<Symbol>& _out);
    static void parseAreas(const QJsonObject& _root, Doc& _doc);
    static void scanSource(const QString& _text, QList<Symbol>& _out);

    QString m_projectDir;
    QStringList m_path;
    QStringList m_docpath;
    QStringList m_userDirs;
    int m_limit = 64;
    QHash<QString, Doc> m_pools[6];
    QList<QPair<int, QString>> m_lru; // pool index, key — most recent at the back
    int m_total = 0;

    QHash<QString, Target> m_links;
    QHash<QString, Target> m_imports;
    quint64 m_aliasHash = 0;
    int m_aliasLength = -1;
};

#endif // HINTPROVIDER_H
