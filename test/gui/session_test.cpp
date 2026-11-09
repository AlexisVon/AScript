// axc-gui-test — headless drive of the Qt session layer against a real axc-runner.
// Qt Core only: no widgets, no display.
#include "BreakpointStore.h"
#include "EngineObject.h"
#include "Settings.h"
#include "debug/PerfSampler.h"
#include "debug/Session.h"
#include "debug/SessionManager.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTimer>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>

#ifndef _WIN32
#    include <unistd.h>
#endif

namespace {
    int g_failures = 0;

    // check — one named expectation
    void check(bool _cond, const char* _what) {
        std::printf("  %s %s\n", _cond ? "ok  " : "FAIL", _what);
        if (!_cond) ++g_failures;
    }

    // quiet — runner diagnostics only with VERBOSE
    void log_line(const QString& _text) {
        if (std::getenv("VERBOSE")) std::fprintf(stderr, "  [session] %s\n", _text.toUtf8().constData());
    }

    // recorder — collects what the session reported
    struct recorder {
        QString output;
        QString message;
        int error = 0;
        bool got_result = false;
        bool got_finished = false;
        bool ok = false;
    };

    // watch — connect a session's signals into a recorder
    void watch(Session& _s, recorder& _r) {
        QObject::connect(&_s, &Session::outputText, [&_r](const QString& _t) { _r.output += _t; });
        QObject::connect(&_s, &Session::execResult,
                         [&_r](const QString&, qint64, bool _ok, int _err) {
                             _r.got_result = true;
                             _r.ok = _ok;
                             _r.error = _err;
                         });
        QObject::connect(&_s, &Session::finished, [&_r](bool _ok, const QString& _msg) {
            _r.got_finished = true;
            _r.ok = _ok;
            _r.message = _msg;
        });
        QObject::connect(&_s, &Session::logMessage, [](const QString& _t) { log_line(_t); });
    }

    // wait_until — pump the event loop until the predicate holds or the deadline
    bool wait_until(const std::function<bool()>& _pred, int _timeout_ms) {
        if (_pred()) return true;
        QEventLoop loop;
        QTimer poll;
        poll.setInterval(10);
        QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
            if (_pred()) loop.quit();
        });
        QTimer::singleShot(_timeout_ms, &loop, &QEventLoop::quit);
        poll.start();
        loop.exec();
        return _pred();
    }

    // wait_finished — run the event loop until finished or the deadline
    bool wait_finished(Session& _s, int _timeout_ms) {
        if (!_s.isRunning()) return true;
        QEventLoop loop;
        QObject::connect(&_s, &Session::finished, &loop, &QEventLoop::quit);
        QTimer::singleShot(_timeout_ms, &loop, &QEventLoop::quit);
        loop.exec();
        return !_s.isRunning();
    }

    // start — fresh session with the runner path from main
    bool start(Session& _s, const QString& _runner, const QString& _code, const QString& _path,
               const QString& _cwd, bool _debug = false) {
        _s.setRunnerPath(_runner);
        return _s.run(_code, _path, _cwd, _debug);
    }

    // test_text — one print round trip
    void test_text(const QString& _runner) {
        std::printf("text: one print\n");
        Session s;
        recorder r;
        watch(s, r);
        check(start(s, _runner, R"($io_print("qt hi");)", QString(), QDir::currentPath()), "session starts");
        check(wait_finished(s, 10000), "session finishes");
        check(r.got_result && r.ok, "result is ok");
        check(r.output == "qt hi", "output is byte exact");
        check(r.got_finished && r.ok, "finished reports ok");
    }

    // test_stop — interrupt an infinite loop
    void test_stop(const QString& _runner) {
        std::printf("stop: infinite loop\n");
        Session s;
        recorder r;
        watch(s, r);
        check(start(s, _runner, "while (true) { }", QString(), QDir::currentPath()), "session starts");
        QTimer::singleShot(300, &s, [&s] { s.stop(); });
        check(wait_finished(s, 5000), "session stops");
        check(r.got_result && !r.ok && r.error == 1, "result is InterruptedError");
        check(r.got_finished && !r.ok, "finished reports failure");
    }

    // test_stop_early — stop before the exec command goes out must not run the script
    void test_stop_early(const QString& _runner) {
        std::printf("stop: before exec\n");
        Session s;
        recorder r;
        watch(s, r);
        check(start(s, _runner, R"($io_print("should not run");)", QString(), QDir::currentPath()), "session starts");
        s.stop();
        check(wait_finished(s, 5000), "early stop finishes");
        check(r.output.isEmpty(), "script did not run");
        check(r.got_finished && !r.ok, "early stop reports failure");
    }

    // test_compile — a syntax error surfaces as compileError + ParseError
    void test_compile(const QString& _runner) {
        std::printf("compile: syntax error\n");
        Session s;
        recorder r;
        watch(s, r);
        check(start(s, _runner, "var x = ;", QString(), QDir::currentPath()), "session starts");
        check(wait_finished(s, 10000), "session finishes");
        check(r.output.contains(':') && r.output.contains("Unexpected"), "compile error text arrives");
        check(r.got_result && !r.ok && r.error == 11, "result is ParseError");
    }

    // test_input — pipe_in round trip through the GUI session
    void test_input(const QString& _runner) {
        std::printf("input: one line\n");
        Session s;
        recorder r;
        watch(s, r);
        QObject::connect(&s, &Session::inputRequested, [&s] { s.submitInput(QStringLiteral("beta")); });
        check(start(s, _runner, "var x = $io_input();\n$io_print(\"got \" + x);", QString(), QDir::currentPath()),
              "session starts");
        check(wait_finished(s, 10000), "session finishes");
        check(r.output == "got beta", "input reaches the script");
        check(r.got_result && r.ok, "input run is ok");
    }

    // write_file — small helper for the temp fixtures
    bool write_file(const QString& _path, const QByteArray& _data) {
        QFile f(_path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        return f.write(_data) == _data.size();
    }

    // test_project — .ax entry: the runner parses the project and runs its main
    void test_project(const QString& _runner) {
        std::printf("project: .ax entry\n");
        const QString dir = QDir::tempPath() + QStringLiteral("/axc-gui-test-ax");
        QDir().mkpath(dir);
        if (!write_file(dir + QStringLiteral("/main.axc"), QByteArray(R"($io_print("project");)")) ||
            !write_file(dir + QStringLiteral("/proj.ax"), QByteArray("{\"main\": \"main.axc\"}"))) {
            check(false, "project fixture written");
            return;
        }
        Session s;
        recorder r;
        watch(s, r);
        check(start(s, _runner, QString(), dir + QStringLiteral("/proj.ax"), dir), "session starts");
        check(wait_finished(s, 10000), "session finishes");
        check(r.output == "project", "project output");
        check(r.got_result && r.ok, "project run is ok");
        QFile::remove(dir + QStringLiteral("/main.axc"));
        QFile::remove(dir + QStringLiteral("/proj.ax"));
    }


    // debug_rec — what a debug session reported
    struct debug_rec {
        int stops = 0;
        int line = 0;
        int col = 0;
        QString file;
        QVariantList frames; // breakpointFrames: call stack, innermost first
        QVariantList root; // first page of the paused frame's snapshot
        bool have_root = false;
        bool paused = false;
        QVariantList threads; // threadsChanged: captured threads of the stop wave
        int known = 0;
        int captured = 0;
        quint64 focus = 0;
    };

    // watch_debug — connect the debug signals into a recorder
    void watch_debug(Session& _s, debug_rec& _d) {
        QObject::connect(&_s, &Session::paused, [&_d] {
            _d.paused = true;
            ++_d.stops;
        });
        QObject::connect(&_s, &Session::breakpointHit,
                         [&_d](const QString& _file, int _line, int _col) {
                             _d.file = _file;
                             _d.line = _line;
                             _d.col = _col;
                         });
        QObject::connect(&_s, &Session::breakpointFrames,
                         [&_d](const QVariantList& _frames) { _d.frames = _frames; });
        QObject::connect(&_s, &Session::snapshotReady,
                         [&_d, &_s](const QString& _ref, quint64 _ofst, const QVariantList& _nodes, bool) {
                             if (_ofst == 0 && _ref == _s.snapshot_ref()) {
                                 _d.root = _nodes;
                                 _d.have_root = true;
                             }
                         });
        QObject::connect(&_s, &Session::threadsChanged,
                         [&_d, &_s](const QVariantList& _threads, quint64, int _known, int _captured) {
                             _d.threads = _threads;
                             _d.known = _known;
                             _d.captured = _captured;
                             _d.focus = _s.focusThread();
                         });
    }

    // test_debug_breakpoint — a line breakpoint pauses with the root locals
    void test_debug_breakpoint(const QString& _runner) {
        std::printf("debug: line breakpoint\n");
        Session s;
        recorder r;
        debug_rec d;
        watch(s, r);
        watch_debug(s, d);
        s.setRunnerPath(_runner);
        s.set_breakpoints({{"buf:run", 3, true}});
        const QString code = "var a = 1;\nvar b = 2;\nvar c = a + b;\n$io_print(string(c));";
        check(s.run(code, QString(), QDir::currentPath(), true), "debug session starts");
        check(wait_until([&] { return d.stops == 1 && d.have_root; }, 10000), "paused with a snapshot");
        check(d.line == 3, "paused on line 3");
        check(!d.frames.isEmpty(), "frame list arrives");
        if (!d.frames.isEmpty()) {
            const QVariantMap top = d.frames.first().toMap();
            check(!top.value(QStringLiteral("ref")).toString().isEmpty(), "innermost frame carries a ref");
            check(top.value(QStringLiteral("line")).toInt() == 3, "innermost frame carries its line");
        }
        QString a, b;
        for (const auto& nv : d.root) {
            const QVariantMap n = nv.toMap();
            if (n.value(QStringLiteral("name")) == QLatin1String("a")) a = n.value(QStringLiteral("value")).toString();
            if (n.value(QStringLiteral("name")) == QLatin1String("b")) b = n.value(QStringLiteral("value")).toString();
        }
        check(a == QLatin1String("1") && b == QLatin1String("2"), "root locals are listed");

        s.continue_run();
        check(wait_finished(s, 10000), "debug run finishes");
        check(r.output == "3", "debug run output");
        check(r.got_result && r.ok, "debug run is ok");
    }

    // test_debug_push_breakpoint — a breakpoint added during the run takes effect
    void test_debug_push_breakpoint(const QString& _runner) {
        std::printf("debug: push breakpoint mid-run\n");
        Session s;
        recorder r;
        debug_rec d;
        watch(s, r);
        watch_debug(s, d);
        s.setRunnerPath(_runner);
        const QString path = QDir::tempPath() + QStringLiteral("/axc-gui-push.axc");
        if (!write_file(path, "var i = 0;\nwhile (i < 5000000) { i = i + 1; }\n$io_print(string(i));\n")) {
            check(false, "push-breakpoint fixture written");
            return;
        }
        const std::string source = "file:" + QFileInfo(path).canonicalFilePath().toStdString();
        check(s.run(QString(), path, QDir::tempPath(), true), "debug session starts");
        check(wait_until([&] { return s.current() == Session::state::running; }, 5000), "exec is sent");
        s.set_breakpoints({{source, 3, true}});
        s.push_breakpoints(path);
        check(wait_until([&] { return d.stops == 1; }, 15000), "mid-run push stops the run");
        check(d.line == 3, "paused on line 3");
        s.continue_run();
        check(wait_finished(s, 15000), "debug run finishes");
        check(r.output == "5000000", "push-breakpoint run output");
        QFile::remove(path);
    }

    // test_debug_step — stop on entry, then step over one statement
    void test_debug_step(const QString& _runner) {
        std::printf("debug: step over\n");
        Session s;
        recorder r;
        debug_rec d;
        watch(s, r);
        watch_debug(s, d);
        s.setRunnerPath(_runner);
        s.set_stop_on_entry(true);
        check(s.run("var a = 1;\nvar b = 2;", QString(), QDir::currentPath(), true), "debug session starts");
        check(wait_until([&] { return d.stops == 1; }, 10000), "entry stop arrives");
        check(d.line == 1, "entry is line 1");

        s.step_over();
        check(wait_until([&] { return d.stops == 2; }, 10000), "step stop arrives");
        check(d.line == 2, "stepped to line 2");

        s.continue_run();
        check(wait_finished(s, 10000), "step run finishes");
        check(r.got_result && r.ok, "step run is ok");
    }

    // test_debug_run_to_cursor — temporary breakpoint at a later line
    void test_debug_run_to_cursor(const QString& _runner) {
        std::printf("debug: run to cursor\n");
        const QString dir = QDir::tempPath() + QStringLiteral("/axc-gui-test-rtc");
        QDir().mkpath(dir);
        const QString path = dir + QStringLiteral("/rtc.axc");
        if (!write_file(path, QByteArray("var a = 1;\nvar b = 2;\nvar c = a + b;\n$io_print(string(c));\n"))) {
            check(false, "run-to-cursor fixture written");
            return;
        }
        Session s;
        recorder r;
        debug_rec d;
        watch(s, r);
        watch_debug(s, d);
        s.setRunnerPath(_runner);
        s.set_stop_on_entry(true);
        check(s.run(QString(), path, dir, true), "debug session starts");
        check(wait_until([&] { return d.stops == 1; }, 10000), "entry stop arrives");
        check(d.line == 1, "entry is line 1");

        s.run_to_line(path, 3);
        check(wait_until([&] { return d.stops == 2; }, 10000), "run-to-cursor stop arrives");
        check(d.line == 3, "stopped at the cursor line");

        s.continue_run();
        check(wait_finished(s, 10000), "run-to-cursor run finishes");
        check(r.output == "3", "run-to-cursor output");

        // the temporary breakpoint must be gone: a second stop-on-entry run still runs
        QFile::remove(path);
    }

    // test_debug_snapshot_tree — expand a map through snapshot requests
    void test_debug_snapshot_tree(const QString& _runner) {
        std::printf("debug: expand a snapshot\n");
        Session s;
        recorder r;
        debug_rec d;
        watch(s, r);
        watch_debug(s, d);
        QVariantList root_nodes;
        QVariantList map_nodes;
        QObject::connect(&s, &Session::snapshotReady,
                         [&](const QString& _ref, quint64, const QVariantList& _nodes, bool) {
                             if (_ref == QLatin1String("root")) root_nodes = _nodes;
                             if (_ref == QLatin1String("root/m")) map_nodes = _nodes;
                         });
        s.setRunnerPath(_runner);
        s.set_breakpoints({{"buf:run", 3, true}});
        check(s.run("var m = map{\"a\": 1, \"b\": 2};\nvar n = 3;\n$io_print(string(m[\"a\"]));", QString(),
                    QDir::currentPath(), true),
              "debug session starts");
        check(wait_until([&] { return d.stops == 1 && !root_nodes.isEmpty(); }, 10000), "root snapshot arrives");

        QVariantMap map_node;
        for (const auto& nv : root_nodes) {
            const QVariantMap n = nv.toMap();
            if (n.value(QStringLiteral("name")) == QLatin1String("m")) map_node = n;
        }
        check(!map_node.isEmpty(), "map local is listed");
        check(map_node.value(QStringLiteral("hasChildren")).toBool(), "map is expandable");

        s.request_snapshot(QStringLiteral("root/m"));
        check(wait_until([&] { return map_nodes.size() == 2; }, 10000), "map children arrive");
        QStringList names;
        for (const auto& nv : map_nodes) names << nv.toMap().value(QStringLiteral("name")).toString();
        names.sort();
        check(names == QStringList({QStringLiteral("a"), QStringLiteral("b")}), "map keys are a and b");

        s.continue_run();
        check(wait_finished(s, 10000), "snapshot run finishes");
        check(r.output == "1", "snapshot run output");
    }

    // test_debug_view — a cv handle answers desc/view through the session
    void test_debug_view(const QString& _runner) {
        std::printf("debug: view a handle\n");
        if (!QFile::exists(QDir::currentPath() + QStringLiteral("/bin/links/cv.so"))) {
            std::printf("  skipped: bin/links/cv.so is not built\n");
            return;
        }
        Session s;
        recorder r;
        debug_rec d;
        watch(s, r);
        watch_debug(s, d);
        QVariantMap view;
        QString view_ref;
        QString view_error;
        bool view_ok = false;
        QObject::connect(&s, &Session::viewReady,
                         [&](const QString& _ref, bool _ok, const QString& _error, const QVariantMap& _v) {
                             view_ref = _ref;
                             view_ok = _ok;
                             view_error = _error;
                             view = _v;
                         });
        s.setRunnerPath(_runner);
        s.set_breakpoints({{"buf:run", 4, true}});
        check(s.run("link \"bin/links/cv\" as cv;\nvar m = cv.create(32, 16, \"8uc3\", 0x00FF00);\n"
                    "var x = 7;\n$io_print(\"ok\");",
                    QString(), QDir::currentPath(), true),
              "view session starts");
        check(wait_until([&] { return d.stops == 1; }, 10000), "view stop arrives");

        s.request_view(QStringLiteral("root/m"));
        check(wait_until([&] { return view_ok || !view_error.isEmpty(); }, 10000), "view answer arrives");
        check(view_ref == QLatin1String("root/m"), "view ref echoes");
        check(view_ok, "view ok");
        check(view.value(QStringLiteral("kind")).toString() == QLatin1String("image"),
              "view kind is image");
        check(view.value(QStringLiteral("format")).toString() == QLatin1String("bmp"),
              "view format is bmp");
        const QByteArray bmp = view.value(QStringLiteral("data")).toByteArray();
        check(bmp.size() > 2 && bmp.at(0) == 'B' && bmp.at(1) == 'M', "view data is a BMP");

        view_ok = false;
        view_error.clear();
        s.request_view(QStringLiteral("root/nope"));
        check(wait_until([&] { return !view_ok && !view_error.isEmpty(); }, 10000),
              "missing ref fails");
        check(view_error.contains(QLatin1String("does not resolve")), "missing ref names the failure");

        s.continue_run();
        check(wait_finished(s, 10000), "view run finishes");
        check(r.output == "ok", "view run output");
    }

    // test_debug_modules — the module snapshot lists imported modules
    void test_debug_modules(const QString& _runner) {
        std::printf("debug: module tree\n");
        const QString dir = QDir::tempPath() + QStringLiteral("/axc-gui-test-mod");
        QDir().mkpath(dir);
        const QString mod = dir + QStringLiteral("/mod1.axc");
        const QString main = dir + QStringLiteral("/main.axc");
        if (!write_file(mod, QByteArray("var z = 9;\n")) ||
            !write_file(main, QByteArray("import \"mod1.axc\" as mod1;\nvar a = 1;\n$io_print(string(a));\n"))) {
            check(false, "module fixture written");
            return;
        }
        Session s;
        recorder r;
        debug_rec d;
        watch(s, r);
        watch_debug(s, d);
        QVariantList modules;
        QObject::connect(&s, &Session::snapshotReady,
                         [&](const QString& _ref, quint64, const QVariantList& _nodes, bool) {
                             if (_ref == QLatin1String("modules")) modules = _nodes;
                         });
        s.setRunnerPath(_runner);
        s.set_breakpoints({{"file:" + QFileInfo(main).canonicalFilePath().toStdString(), 3, true}});
        check(s.run(QString(), main, dir, true), "debug session starts");
        check(wait_until([&] { return d.stops == 1; }, 10000), "module breakpoint stops");
        s.request_snapshot(QStringLiteral("modules"));
        check(wait_until([&] { return !modules.isEmpty(); }, 10000), "module snapshot arrives");

        bool found = false;
        for (const auto& nv : modules) {
            const QVariantMap n = nv.toMap();
            if (n.value(QStringLiteral("name")) == QLatin1String("mod1")) {
                found = true;
                check(n.value(QStringLiteral("value")).toString().endsWith(QLatin1String("mod1.axc")),
                      "module path points at mod1.axc");
            }
        }
        check(found, "imported module is listed");

        s.continue_run();
        check(wait_finished(s, 10000), "module run finishes");
        QFile::remove(mod);
        QFile::remove(main);
    }

    // test_log_retention — stale session logs are purged on the next run
    void test_log_retention(const QString& _runner) {
        std::printf("log: retention\n");
        const QString stale = QDir::temp().filePath(QStringLiteral("axc-session-stale.log"));
        QFile old(stale);
        if (old.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            old.write("old");
            old.close();
        }
        QFile touch(stale);
        if (touch.open(QIODevice::ReadOnly))
            touch.setFileTime(QDateTime::currentDateTime().addDays(-30), QFileDevice::FileModificationTime);
        check(QFile::exists(stale), "stale fixture exists");
        check(QFileInfo(stale).lastModified().daysTo(QDateTime::currentDateTime()) > 7, "fixture is old");

        Session s;
        recorder r;
        watch(s, r);
        s.setRunnerPath(_runner);
        check(s.run(QStringLiteral("1;"), QString(), QDir::currentPath(), false), "retention session starts");
        check(wait_finished(s, 10000), "retention session finishes");
        check(!QFile::exists(stale), "stale session log is removed");
        QFile::remove(s.logPath());
    }

    // test_multi_session — three sessions, one crash, the others unharmed
    void test_multi_session(const QString& _runner) {
#ifndef _WIN32
        std::printf("multi: concurrent sessions\n");
        SessionManager mgr;
        mgr.setRunnerPath(_runner);
        mgr.setMax(3);
        QSet<int> done;
        QMap<int, QString> outs;
        QObject::connect(&mgr, &SessionManager::finished, [&](int _id) { done.insert(_id); });

        const int a = mgr.start(QStringLiteral(R"($io_print("a");)"), QString(), QDir::currentPath(), false);
        const int b = mgr.start(QStringLiteral("while (true) { }"), QString(), QDir::currentPath(), false);
        const int c = mgr.start(QStringLiteral(R"($io_print("c");)"), QString(), QDir::currentPath(), false);
        check(a > 0 && b > 0 && c > 0, "three sessions start");
        check(mgr.count() == 3 && mgr.activeId() == a, "the first session is active");
        check(mgr.start(QStringLiteral("1;"), QString(), QDir::currentPath(), false) == -1, "the cap refuses a fourth");

        for (int id : {a, b, c}) {
            QObject::connect(mgr.at(id), &Session::outputText, [&outs, id](const QString& _t) { outs[id] += _t; });
        }
        check(wait_until([&] { return done.contains(a) && done.contains(c); }, 15000), "both quick sessions finish");
        check(outs[a] == QLatin1String("a") && outs[c] == QLatin1String("c"), "each session gets its own output");

        check(mgr.at(b) && mgr.at(b)->pid() != 0, "the long session has a pid");
        ::kill(static_cast<int>(mgr.at(b)->pid()), SIGKILL);
        check(wait_until([&] { return done.contains(b); }, 5000), "the killed session ends");
        check(mgr.at(a) && !mgr.at(a)->isRunning() && mgr.at(c) && !mgr.at(c)->isRunning(),
              "the neighbours stay finished, not crashed");
        mgr.closeAll();
#endif
    }

    // test_console_session — one process runs REPL lines and keeps state
    void test_console_session(const QString& _runner) {
        std::printf("console: session lines\n");
        Session s;
        recorder r;
        watch(s, r);
        int results = 0;
        QObject::connect(&s, &Session::execResult,
                         [&results](const QString&, qint64, bool, int) { ++results; });
        s.setRunnerPath(_runner);
        check(s.start_console(QDir::currentPath()), "console session starts");
        check(wait_until([&] { return s.current() == Session::state::console; }, 5000), "console is ready");

        check(s.console_exec(QStringLiteral("var x = 1;")), "first console exec");
        check(wait_until([&] { return results == 1 && s.current() == Session::state::console; }, 5000),
              "first exec result");
        check(s.console_exec(QStringLiteral("x = x + 1;\n$io_print(string(x));")), "second console exec");
        check(wait_until([&] { return results == 2 && r.output == QLatin1String("2"); }, 5000),
              "console keeps the script state");

        s.close_console();
        check(wait_finished(s, 5000), "console closes");
        check(r.got_finished && r.ok, "console close is a clean finish");
    }

    // test_wall_limit — the watchdog interrupts a run past its budget
    void test_wall_limit(const QString& _runner) {
        std::printf("limits: wall clock\n");
        Session s;
        recorder r;
        watch(s, r);
        s.setRunnerPath(_runner);
        s.set_limits(400, 0);
        check(s.run(QStringLiteral("while (true) { }"), QString(), QDir::currentPath(), false),
              "limited session starts");
        check(wait_finished(s, 5000), "watchdog ends the run");
        check(r.got_result && r.error == 1, "watchdog reports InterruptedError");
    }

    // test_session_log — every output byte is also written to the session log
    void test_session_log(const QString& _runner) {
        std::printf("log: raw session log\n");
        Session s;
        recorder r;
        watch(s, r);
        s.setRunnerPath(_runner);
        check(s.run(QStringLiteral("$io_print(\"log me\");"), QString(), QDir::currentPath(), false),
              "log session starts");
        check(wait_finished(s, 10000), "log session finishes");
        check(r.output == "log me", "log run output");

        const QString path = s.logPath();
        check(!path.isEmpty(), "session log has a path");
        QFile f(path);
        bool opened = f.open(QIODevice::ReadOnly);
        check(opened && f.readAll() == QByteArray("log me"), "session log keeps the raw bytes");
        f.close();
        QFile::remove(path);
    }

    // test_crash_gui — a runner SIGKILL is classified and the session closes
    void test_crash_gui(const QString& _runner) {
#ifndef _WIN32
        std::printf("crash: runner SIGKILL\n");
        Session s;
        recorder r;
        watch(s, r);
        s.setRunnerPath(_runner);
        check(s.run(QStringLiteral("while (true) { }"), QString(), QDir::currentPath(), false),
              "crash session starts");
        check(wait_until([&] { return s.current() == Session::state::running && s.pid() != 0; }, 5000),
              "runner is running");
        ::kill(static_cast<int>(s.pid()), SIGKILL);
        check(wait_finished(s, 5000), "crash ends the session");
        check(r.got_finished && !r.ok, "crash reports failure");
        check(r.message.contains(QStringLiteral("崩溃")) || r.message.contains(QStringLiteral("异常")),
              "crash is classified");
#endif
    }

    // test_debug_pause — pause a running loop, then stop from the pause
    void test_debug_pause(const QString& _runner) {
        std::printf("debug: pause while running\n");
        Session s;
        recorder r;
        debug_rec d;
        watch(s, r);
        watch_debug(s, d);
        s.setRunnerPath(_runner);
        check(s.run("var i = 0;\nwhile (true) { i++; }", QString(), QDir::currentPath(), true),
              "debug session starts");
        check(wait_until([&] { return s.current() == Session::state::running; }, 5000), "session is running");
        s.pause_run();
        check(wait_until([&] { return d.stops == 1; }, 5000), "pause stop arrives");

        s.stop();
        check(wait_finished(s, 5000), "stop ends the paused run");
        check(r.got_result && !r.ok && r.error == 1, "paused stop is InterruptedError");
    }

    // test_debug_stop_paused — stop while paused at entry
    void test_debug_stop_paused(const QString& _runner) {
        std::printf("debug: stop while paused\n");
        Session s;
        recorder r;
        debug_rec d;
        watch(s, r);
        watch_debug(s, d);
        s.setRunnerPath(_runner);
        s.set_stop_on_entry(true);
        check(s.run("var a = 1;\nvar b = 2;", QString(), QDir::currentPath(), true), "debug session starts");
        check(wait_until([&] { return d.stops == 1; }, 10000), "entry stop arrives");

        const auto t0 = std::chrono::steady_clock::now();
        s.stop();
        check(wait_finished(s, 5000), "paused run stops");
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        check(r.got_result && r.error == 1, "paused stop is InterruptedError");
        check(ms < 2500, "paused stop lands within 2 s");
    }

    // test_missing — a missing runner fails before spawning anything
    void test_missing() {
        std::printf("missing: no runner binary\n");
        Session s;
        s.setRunnerPath(QStringLiteral("/nonexistent/axc-runner"));
        check(!s.run(QStringLiteral("1;"), QString(), QDir::currentPath(), false), "run refuses a missing runner");
    }

    // test_engine_object — the exact GUI path: EngineObject → Session → runner
    void test_engine_object(const QString& _runner) {
        std::printf("engine object: runner backend\n");
        Settings::setRunnerPath(_runner);
        EngineObject obj;
        QString output;
        bool finished = false;
        bool ok = false;
        QObject::connect(&obj, &EngineObject::outputText, [&output](const QString& _t) { output += _t; });
        QObject::connect(&obj, &EngineObject::finished, [&](bool _ok, const QString&) {
            finished = true;
            ok = _ok;
        });
        QObject::connect(&obj, &EngineObject::logMessage, [](const QString& _t) { log_line(_t); });

        obj.runText(QStringLiteral(R"($io_print("via engine object");)"), QString(), false);
        QEventLoop loop;
        QObject::connect(&obj, &EngineObject::finished, &loop, &QEventLoop::quit);
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
        check(output == "via engine object", "engine object output");
        check(finished && ok, "engine object reports ok");
        check(!obj.isRunning(), "engine object is idle again");
        Settings::setRunnerPath(QString());
    }

    // test_protocol_trace — the trace path records every message type
    void test_protocol_trace(const QString& _runner) {
        std::printf("trace: protocol log\n");
        const QString path = QDir::temp().filePath(QStringLiteral("axc-trace-test.log"));
        QFile::remove(path);
        Session s;
        recorder r;
        watch(s, r);
        s.setRunnerPath(_runner);
        s.set_trace_path(path);
        check(s.run(QStringLiteral(R"($io_print("t");)"), QString(), QDir::currentPath(), false),
              "trace session starts");
        check(wait_finished(s, 10000), "trace session finishes");
        QFile f(path);
        const QByteArray data = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        check(data.contains("-> exec") && data.contains("<- result"), "trace records exec and result");
        check(data.contains("<- exited"), "trace records exited");
        check(data.contains("-> hello") && data.contains("-> result"),
              "trace keeps the runner's appended lines");
        QFile::remove(path);
    }

    // test_log_off — the switch suppresses the file and the path
    void test_log_off(const QString& _runner) {
        std::printf("log: disabled\n");
        Session s;
        RunnerProcess::run_options opts;
        opts.log_file = false;
        s.set_run_options(opts);
        recorder r;
        watch(s, r);
        s.setRunnerPath(_runner);
        check(s.run(QStringLiteral(R"($io_print("no file");)"), QString(), QDir::currentPath(), false),
              "log-off session starts");
        check(wait_finished(s, 10000), "log-off session finishes");
        check(r.output == "no file", "log-off output");
        check(s.logPath().isEmpty(), "log-off session has no log path");
    }

    // test_output_flood — a million lines arrive without stalling the event loop
    void test_output_flood(const QString& _runner) {
        std::printf("flood: 1M lines\n");
        Session s;
        RunnerProcess::run_options opts;
        opts.log_file = false; // keep /tmp clean and the flood independent of disk I/O
        s.set_run_options(opts);
        recorder r;
        watch(s, r);
        s.setRunnerPath(_runner);
        const QString code =
            QStringLiteral("var i = 0;\nwhile (i < 1000000) { $io_print(\"ab\\n\"); i++; }\n");
        check(s.run(code, QString(), QDir::currentPath(), false), "flood session starts");
        // start measuring after the synchronous spawn, so setup cost is not a "stall"
        QElapsedTimer gap;
        gap.start();
        qint64 max_gap = 0;
        QTimer probe;
        probe.setInterval(5);
        QObject::connect(&probe, &QTimer::timeout, [&] { max_gap = qMax(max_gap, gap.restart()); });
        probe.start();
        check(wait_finished(s, 60000), "flood session finishes");
        probe.stop();
        check(r.output.size() == 3000000, "flood output is complete");
        check(max_gap < 200, "event loop stays responsive");
    }

    // test_breakpoint_store — logpoint text survives to the wire form
    void test_breakpoint_store() {
        std::printf("breakpoints: logpoint text\n");
        const QString path = QDir::currentPath() + QStringLiteral("/store-test.axc");
        BreakpointStore& store = BreakpointStore::instance();
        store.setLog(path, 3, QStringLiteral("hello"));
        check(store.log(path, 3) == QLatin1String("hello"), "log text is stored");
        bool found = false;
        for (const auto& b : store.to_list())
            if (b.line == 3 && b.log == "hello") found = true;
        check(found, "log text reaches the wire form");
        store.setCondition(path, 3, QStringLiteral("i == 5"));
        check(store.condition(path, 3) == QLatin1String("i == 5"), "condition is stored");
        bool cond = false;
        for (const auto& b : store.to_list())
            if (b.line == 3 && b.condition == "i == 5") cond = true;
        check(cond, "condition reaches the wire form");
        store.setCondition(path, 3, QString());
        check(store.condition(path, 3).isEmpty(), "empty condition clears it");
        store.setLog(path, 3, QString());
        check(store.log(path, 3).isEmpty(), "empty text clears the log");
        store.toggle(path, 3); // drop the row so the test leaves no breakpoint behind
    }

    // test_engine_wall_limit — Settings' budget reaches the runner through EngineObject
    void test_engine_wall_limit() {
        std::printf("limits: engine object wall budget\n");
        Settings::setRunWallMs(400);
        EngineObject obj;
        bool finished = false;
        bool ok = true;
        int error = 0;
        QObject::connect(&obj, &EngineObject::finished, [&](bool _ok, const QString&) {
            finished = true;
            ok = _ok;
        });
        QObject::connect(&obj, &EngineObject::execResult, [&](const QString&, qint64, bool, int _err) {
            error = _err;
        });
        QObject::connect(&obj, &EngineObject::logMessage, [](const QString& _t) { log_line(_t); });
        obj.runText(QStringLiteral("while (true) { }"), QString(), false);
        check(wait_until([&] { return finished; }, 5000), "engine object budget ends the run");
        check(!ok, "engine object reports failure");
        check(error == 1, "engine object sees InterruptedError");
        check(!obj.isRunning(), "engine object is idle again");
        Settings::setRunWallMs(0);
    }

    // test_perf_sampler — samples its own pid, then stops cleanly
    void test_perf_sampler() {
        std::printf("perf: own pid sampling\n");
        PerfSampler p;
        int samples = 0;
        double cpu = -1;
        quint64 rss = 0;
        QObject::connect(&p, &PerfSampler::sampled, [&](double _cpu, quint64 _rss) {
            ++samples;
            cpu = _cpu;
            rss = _rss;
        });
        p.start(static_cast<quint64>(QCoreApplication::applicationPid()));
        check(samples == 1 && rss > 0, "an initial RSS point arrives");
        check(cpu == 0.0, "the initial CPU point is zero");
        check(wait_until([&] { return samples >= 2; }, 3000), "a sampled point follows");
        check(cpu >= 0.0, "CPU is non-negative");
        p.stop();
        check(!p.active(), "stop ends sampling");
    }

    // test_file — run a saved file through the file entry
    void test_file(const QString& _runner) {
        std::printf("file: saved script\n");
        const QString dir = QDir::tempPath() + QStringLiteral("/axc-gui-test");
        QDir().mkpath(dir);
        const QString path = dir + QStringLiteral("/hello.axc");
        {
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                check(false, "temp script written");
                return;
            }
            f.write("$io_print(\"from file\\n\");\n");
        }
        Session s;
        recorder r;
        watch(s, r);
        check(start(s, _runner, QString(), path, dir), "session starts");
        check(wait_finished(s, 10000), "session finishes");
        check(r.output == "from file\n", "file output is byte exact");
        check(r.got_result && r.ok, "file run is ok");
        QFile::remove(path);
    }

    // test_mt_debug — a child trap pauses the session; the child focus refuses step
    void test_mt_debug(const QString& _runner) {
        std::printf("debug: multi-thread child trap\n");
        Session s;
        recorder r;
        debug_rec d;
        watch(s, r);
        watch_debug(s, d);
        QString trapLabel;
        QString trapArgs;
        QObject::connect(&s, &Session::trapReported,
                         [&trapLabel, &trapArgs](const QString& _label, const QString& _args) {
                             trapLabel = _label;
                             trapArgs = _args;
                         });
        s.setRunnerPath(_runner);
        s.set_multi_thread(true);
        const QString code =
            "var h = $thd_create(\"var a = 41;\\ntrap(\\\"child here\\\");\\n$io_print(\\\"child\\\");\");\n"
            "$thd_join(h);\n"
            "$io_print(\"done\");";
        check(s.run(code, QString(), QDir::currentPath(), true), "mt session starts");
        check(wait_until([&] { return d.stops == 1 && d.captured == 1; }, 10000),
              "the child trap pauses the session");
        check(d.focus != 0, "focus starts on the triggering child");
        check(d.known == 2 && d.captured == 1, "one of two engines is captured");
        bool child = false;
        for (const auto& tv : d.threads) {
            const QVariantMap t = tv.toMap();
            if (t.value(QStringLiteral("id")).toULongLong() == 0) continue;
            child = true;
            check(t.value(QStringLiteral("trigger")).toBool(), "the child is the trigger");
            check(t.value(QStringLiteral("label")).toString() == QLatin1String("child here"),
                  "the trap label reaches the session");
        }
        check(child, "the thread table lists the child");
        check(!d.frames.isEmpty(), "the child stack arrives");
        check(trapLabel == QLatin1String("child here"), "the trap label is reported");
        check(trapArgs.contains(QStringLiteral("child here")), "the trap payload is reported");

        s.step_over();
        check(s.isPaused(), "step is ignored on a child focus");
        s.continue_run();
        check(wait_finished(s, 10000), "mt run finishes");
        check(r.output == "childdone", "mt output");
        check(r.got_result && r.ok, "mt run is ok");
    }

    // test_mt_focus — switch from the child to the main thread and step it
    void test_mt_focus(const QString& _runner) {
        std::printf("debug: multi-thread focus switch\n");
        Session s;
        recorder r;
        debug_rec d;
        watch(s, r);
        watch_debug(s, d);
        s.setRunnerPath(_runner);
        s.set_multi_thread(true);
        const QString code =
            "var h = $thd_create(\"trap(\\\"c\\\");\");\n"
            "var i = 0;\n"
            "while (i < 200000) { i++; }\n"
            "$thd_join(h);\n"
            "$io_print(\"done\");";
        check(s.run(code, QString(), QDir::currentPath(), true), "focus session starts");
        check(wait_until([&] { return d.stops == 1 && d.captured == 2; }, 10000),
              "both threads are captured");
        check(d.focus != 0, "focus starts on the child");

        s.set_focus_thread(0);
        check(s.focusThread() == 0, "focus switches to the main thread");
        const int before = d.stops;
        d.line = 0;
        s.step_over();
        check(wait_until([&] { return d.stops == before + 1; }, 10000), "step stop arrives");
        check(d.focus == 0, "the step keeps the main focus");
        check(d.line > 0, "the main frame carries a line");

        s.continue_run();
        check(wait_finished(s, 10000), "focus run finishes");
        check(r.output == "done", "focus run output");
        check(r.got_result && r.ok, "focus run is ok");
    }

    // test_empty_log — a run with no output leaves no session log
    void test_empty_log(const QString& _runner) {
        std::printf("log: no output leaves no file\n");
        Session s;
        recorder r;
        watch(s, r);
        QString announced;
        QStringList messages;
        QObject::connect(&s, &Session::logPathChanged, [&announced](const QString& _p) {
            if (!_p.isEmpty()) announced = _p;
        });
        QObject::connect(&s, &Session::logMessage,
                         [&messages](const QString& _t) { messages.append(_t); });
        RunnerProcess::run_options opts;
        opts.log_file = true;
        s.set_run_options(opts);
        s.setRunnerPath(_runner);
        check(s.run(QStringLiteral("var a = 1;"), QString(), QDir::currentPath(), false),
              "empty session starts");
        check(wait_finished(s, 10000), "empty session finishes");
        check(r.got_result && r.ok, "empty run is ok");
        check(!announced.isEmpty(), "a log path was announced before the run");
        check(s.logPath().isEmpty(), "the log path is cleared at the end");
        check(announced.isEmpty() || !QFile::exists(announced), "no log file was created");
        bool warned = false;
        for (const QString& m : messages)
            if (m.contains(QStringLiteral("无打印输出"))) warned = true;
        check(warned, "the GUI explains why the log is missing");
    }
} // namespace

int main(int _argc, char** _argv) {
    // keep QSettings out of the developer's config even when run without run.sh
    const QString cfgDir = QDir::tempPath() + QStringLiteral("/axc-gui-test-config");
    QDir(cfgDir).removeRecursively();
    QDir().mkpath(cfgDir);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, cfgDir);

    QCoreApplication app(_argc, _argv);
    const QString runner = _argc > 1 ? QString::fromLocal8Bit(_argv[1])
                                     : QCoreApplication::applicationDirPath() + QStringLiteral("/axc-runner");

    test_text(runner);
    test_stop(runner);
    test_stop_early(runner);
    test_compile(runner);
    test_input(runner);
    test_debug_breakpoint(runner);
    test_debug_push_breakpoint(runner);
    test_debug_step(runner);
    test_debug_run_to_cursor(runner);
    test_debug_snapshot_tree(runner);
    test_debug_view(runner);
    test_debug_modules(runner);
    test_crash_gui(runner);
    test_session_log(runner);
    test_log_off(runner);
    test_output_flood(runner);
    test_wall_limit(runner);
    test_console_session(runner);
    test_multi_session(runner);
    test_log_retention(runner);
    test_debug_pause(runner);
    test_debug_stop_paused(runner);
    test_missing();
    test_engine_object(runner);
    test_file(runner);
    test_engine_wall_limit();
    test_perf_sampler();
    test_breakpoint_store();
    test_protocol_trace(runner);
    test_project(runner);
    test_mt_debug(runner);
    test_mt_focus(runner);
    test_empty_log(runner);

    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
