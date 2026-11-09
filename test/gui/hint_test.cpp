// hint_test — HintProvider: alias scan, engine-order resolution, six pools,
// cache limit and manual invalidation. No widgets, no OpenCV needed.
#include "HintProvider.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cstdio>

namespace {
    bool writeFile(const QString& _path, const QByteArray& _data) {
        QDir().mkpath(QFileInfo(_path).absolutePath());
        QFile f(_path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        f.write(_data);
        return true;
    }

    bool has(const QList<CompletionItemData>& _items, const QString& _name) {
        for (const auto& d : _items)
            if (d.name == _name) return true;
        return false;
    }
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    int failed = 0;
    auto check = [&](bool _ok, const char* _what) {
        std::printf("%s %s\n", _ok ? "PASS" : "FAIL", _what);
        if (!_ok) ++failed;
    };

    const QString root = QDir::tempPath() + QStringLiteral("/axc_hint_test");
    QDir(root).removeRecursively();
    QDir().mkpath(root + "/link");
    QDir().mkpath(root + "/import");
    // no .so on disk: link hints resolve the .json only
    writeFile(root + "/link/lib1.json",
              "{\"functions\":{"
              "\"alpha\":[{\"desc\":\"first\",\"param\":[{\"key\":\"x\",\"type\":\"int\",\"desc\":\"x\"}],"
              "\"ret\":{\"type\":\"int\",\"desc\":\"x\"}}],"
              "\"beta\":[]}}");
    writeFile(root + "/link/lib2.json",
              "{\"functions\":{\"list\":[],\"show\":[]},"
              "\"areas\":{"
              "\"*\":{\"functions\":{\"chat\":[{\"desc\":\"chat\",\"param\":[{\"key\":\"input\","
              "\"type\":\"string\",\"desc\":\"x\"}],\"ret\":{\"type\":\"string\",\"desc\":\"x\"}}],"
              "\"alive\":[]}},"
              "\"grp\":{\"functions\":{\"ping\":[]}}}}");
    writeFile(root + "/import/mod1.axc", "def alpha(a) { return a; }\nvar limit = 1;\n");

    {
        HintProvider h;
        h.setProject(root, {}, {});
        const QString text = QStringLiteral("link \"%1/link/lib1.so\" as a;\n").arg(root);
        auto one = h.complete(text, root + "/s.axc", "a", "al");
        check(one.size() == 1 && one[0].name == "alpha", "absolute link: name filter");
        check(one.size() == 1 && one[0].sig == "(x: int) → int", "absolute link: signature");
        check(one.size() == 1 && one[0].source == "lib1.so" && one[0].kind == CompletionItemData::Link,
              "absolute link: source and kind");
        auto all = h.complete(text, root + "/s.axc", "a", "");
        check(all.size() == 2, "absolute link: all functions");
        check(h.complete(text, root + "/s.axc", "a.any", "").isEmpty(), "no areas: deeper chain is empty");
        check(h.size() == 1, "absolute link: one cache entry");
    }

    {
        HintProvider h;
        h.setProject(root, {}, {});
        auto items = h.complete("link \"lib1\" as c;\n", root + "/s.axc", "c", "");
        check(items.size() == 2, "relative link: json without the .so");
        check(!items.isEmpty() && items[0].source == "lib1.so", "relative link: guessed source name");
    }

    {
        HintProvider h;
        h.setProject(root, {}, {});
        const QString text = "link \"lib2\" as o;\n";
        check(h.complete(text, root + "/s.axc", "o", "").size() == 2, "area: root functions at alias.");
        auto fn = h.complete(text, root + "/s.axc", "o.inst", "ch");
        check(fn.size() == 1 && fn[0].name == "chat" && fn[0].source == "lib2.so", "area: wildcard instance");
        check(h.complete(text, root + "/s.axc", "o.inst", "").size() == 2, "area: wildcard all methods");
        check(h.complete(text, root + "/s.axc", "o.grp", "pi").size() == 1, "area: named area");
    }

    {
        HintProvider h;
        h.setProject(root, {}, {});
        const QString text = "import \"mod1\" as m;\n";
        auto def = h.complete(text, root + "/s.axc", "m", "alp");
        check(def.size() == 1 && def[0].name == "alpha" && def[0].call, "import: source def");
        check(def.size() == 1 && def[0].sig == "(a)", "import: def signature");
        auto var = h.complete(text, root + "/s.axc", "m", "lim");
        check(var.size() == 1 && var[0].name == "limit" && !var[0].call, "import: var is not callable");
        check(var.size() == 1 && var[0].source == "mod1.axc" && var[0].kind == CompletionItemData::Import,
              "import: source and kind");
        check(h.complete(text, root + "/s.axc", "nope", "").isEmpty(), "unknown alias");
    }

    {
        HintProvider h;
        h.setProject(root, {}, {});
        auto items = h.complete("link \"lib1\" as c;\nvar c = 1;\n", root + "/s.axc", "c", "");
        check(items.size() == 2, "shadowed alias still hints");
    }

    {
        writeFile(root + "/import/bad.axp", "def fake() {}\n"); // not a product
        HintProvider h;
        h.setProject(root, {}, {});
        check(h.complete("import \"bad\" as b;\n", root + "/s.axc", "b", "").isEmpty(),
              "corrupt axp: no garbage hints");
    }

    {
        HintProvider h;
        h.setProject(root, {}, {});
        h.setLimit(1);
        h.complete("link \"lib1\" as c;\n", root + "/s.axc", "c", "");
        h.complete("import \"mod1\" as m;\n", root + "/s.axc", "m", "");
        check(h.size() == 1, "global LRU limit");
    }

    {
        HintProvider h;
        h.setProject(root, {}, {});
        const QString text = "import \"mod1\" as m;\n";
        check(h.complete(text, root + "/s.axc", "m", "extra").isEmpty(), "no diff: absent before");
        writeFile(root + "/import/mod1.axc", "def extra() {}\n");
        check(h.complete(text, root + "/s.axc", "m", "extra").isEmpty(), "no diff: cache is authoritative");
        h.clear();
        check(h.complete(text, root + "/s.axc", "m", "extra").size() == 1, "clear: reloads from disk");
    }

    {
        HintProvider h;
        h.setProject(root, {}, {});
        QList<QPair<QString, QString>> docs;
        docs.append({root + "/a.axc", "link \"lib1\" as c;\n"});
        docs.append({root + "/b.axc", "import \"mod1\" as m;\n"});
        h.reload(docs);
        check(h.size() == 2, "reload: both docs warmed");
        check(has(h.complete("link \"lib1\" as c;\n", root + "/a.axc", "c", "bet"), "beta"),
              "reload: cached complete");
    }

    QDir(root).removeRecursively();
    std::printf(failed ? "FAILED %d\n" : "ALL PASS\n", failed);
    return failed ? 1 : 0;
}
