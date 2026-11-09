#include "gui.h"

#include <iostream>

#include "QScriptMain.h"
#include "QtCompat.h"
#include "Settings.h"

#include <QApplication>
#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>

namespace gui {

    int impl(int _argc, char** _argv) {
        // the platform plugin reads the factor once at startup — must precede QApplication;
        // an explicit QT_SCALE_FACTOR in the environment wins, like the other settings
        const int scale = Settings::uiScalePercent();
        if (scale != 100 && qgetenv("QT_SCALE_FACTOR").isEmpty())
            qputenv("QT_SCALE_FACTOR", QByteArray::number(scale / 100.0));

        // Qt5 has high-DPI scaling off by default; the attribute must precede QApplication
        AXC_CORE_APPLICATION_ENABLE_HIGHDPI();

        // -ui is argv[1] here; a following non-option arg opens in the editor
        QApplication a(_argc, _argv);

        QFontDatabase::addApplicationFont(":/ui/FiraCode.ttf");

        QScriptMain w;
        // widget-level theme (main window + descendants; app-level reparsed the
        // sheet on every style repolish and warned spuriously)
        QFile qss(":/ui/theme.qss");
        if (qss.open(QIODevice::ReadOnly))
            w.setStyleSheet(QString::fromUtf8(qss.readAll()));
        // options first, so --runner-trace is honoured wherever it appears
        QString tracePath;
        QString openPath;
        for (int i = 1; i < _argc; i++) {
            const QString arg = QString::fromLocal8Bit(_argv[i]);
            if (arg == QLatin1String("--runner-trace")) {
                if (i + 1 < _argc) tracePath = QString::fromLocal8Bit(_argv[++i]);
                else std::cerr << "AScript: --runner-trace needs a value\n";
                continue;
            }
            if (arg.startsWith(QLatin1Char('-')) || !openPath.isEmpty()) continue;
            openPath = arg;
        }
        if (!tracePath.isEmpty()) w.setRunnerTracePath(tracePath);
        if (!openPath.isEmpty()) {
            QFileInfo info(openPath);
            if (info.isDir())
                w.openDir(openPath);
            else if (info.suffix() == "ax")
                w.openProject(openPath);
            else if (info.suffix() == "axp")
                w.openAxpView(openPath); // read-only structure dock (no dir context)
            else
                w.openFile(openPath);
        }
        w.show(); // no arg: no docs open (tabs appear on open/new)
        return QApplication::exec();
    }


    void help() {
        std::cout << "AScript - GUI mode\n"
                  << "usage: AScript [-ui] [--runner-trace <file>] [file|dir]\n"
                  << "  (no args)             open GUI (no doc tabs)\n"
                  << "  -ui                   open GUI\n"
                  << "  -ui <file.axc>        open script in the editor\n"
                  << "  -ui <dir>             open directory tree (files dock)\n"
                  << "  --runner-trace <file> record GUI↔runner protocol messages\n"
                  << "  -h                    show global help\n";
    }

} // namespace gui
