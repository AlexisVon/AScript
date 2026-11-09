#include "Settings.h"

#include "proto/config.h"

#include "core/project.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QSettings>

namespace {

    // config file: ~/.config/AScript/AScript.conf
    QSettings& q() {
        static QSettings s(QStringLiteral("AScript"), QStringLiteral("AScript"));
        return s;
    }

    // frame_number — 0 or an in-range value; a stale value falls back to the default
    alx::uint_64 frame_number(const char* _key, alx::uint_64 _def, alx::uint_64 _lo, alx::uint_64 _hi) {
        const alx::uint_64 v =
            q().value(QLatin1String(_key), static_cast<qulonglong>(_def)).toULongLong();
        return v == 0 || (v >= _lo && v <= _hi) ? v : _def;
    }

} // namespace

QString Settings::editorFont() {
    return q().value(QStringLiteral("editor/fontFamily"), QStringLiteral("Fira Code")).toString();
}

int Settings::editorFontSize() {
    return q().value(QStringLiteral("editor/fontSize"), 9).toInt();
}

int Settings::outputFontSize() {
    return q().value(QStringLiteral("output/fontSize"), 9).toInt();
}

int Settings::uiScalePercent() {
    const int pct = q().value(QStringLiteral("ui/scale"), 100).toInt();
    switch (pct) {
    case 100:
    case 125:
    case 150:
    case 175:
    case 200: return pct;
    default: return 100; // only the settings page writes this; anything else is stale
    }
}

void Settings::setUiScalePercent(int _pct) {
    q().setValue(QStringLiteral("ui/scale"), _pct);
}

QStringList Settings::userLibraryDirs() {
    return q().value(QStringLiteral("lib/userDirs")).toStringList();
}

void Settings::setUserLibraryDirs(const QStringList& _dirs) {
    q().setValue(QStringLiteral("lib/userDirs"), _dirs);
}

QStringList Settings::systemLibraryDirs() {
    QStringList out;
    for (const auto& p : core::project::system_library_paths())
        out << QString::fromStdString(p);
    return out;
}

bool Settings::completionEnabled() {
    return q().value(QStringLiteral("completion/enabled"), true).toBool();
}

int Settings::completionDelay() {
    return q().value(QStringLiteral("completion/delay"), 300).toInt();
}

int Settings::completionWindowItems() {
    return q().value(QStringLiteral("completion/windowItems"), 8).toInt();
}

int Settings::completionListLimit() {
    return q().value(QStringLiteral("completion/listLimit"), 32).toInt();
}

int Settings::completionCacheLimit() {
    return q().value(QStringLiteral("completion/cacheLimit"), 64).toInt();
}

void Settings::setCompletionCacheLimit(int _entries) {
    q().setValue(QStringLiteral("completion/cacheLimit"), qMax(0, _entries));
}

bool Settings::completionShowDesc() {
    return q().value(QStringLiteral("completion/showDesc"), true).toBool();
}

int Settings::completionFontSize() {
    return q().value(QStringLiteral("completion/fontSize"), 9).toInt();
}

bool Settings::hoverEnabled() {
    return q().value(QStringLiteral("hover/enabled"), true).toBool();
}

int Settings::hoverFontSize() {
    return q().value(QStringLiteral("hover/fontSize"), 9).toInt();
}

QStringList Settings::recentPaths() {
    QStringList out;
    for (const QString& p : q().value(QStringLiteral("recent/files")).toStringList())
        if (QFileInfo::exists(p)) out.append(p);
    return out.mid(0, 10);
}

void Settings::addRecentPath(const QString& _path) {
    if (_path.isEmpty()) return;
    const QString abs = QFileInfo(_path).absoluteFilePath();
    QStringList list = q().value(QStringLiteral("recent/files")).toStringList();
    list.removeAll(abs);
    list.prepend(abs);
    while (list.size() > 10) list.removeLast();
    q().setValue(QStringLiteral("recent/files"), list);
}

bool Settings::compileDebug() {
    return q().value(QStringLiteral("compile/debug"), false).toBool();
}

bool Settings::compileEmbed() {
    return q().value(QStringLiteral("compile/embed"), false).toBool();
}

void Settings::setCompileDebug(bool _on) {
    q().setValue(QStringLiteral("compile/debug"), _on);
}

void Settings::setCompileEmbed(bool _on) {
    q().setValue(QStringLiteral("compile/embed"), _on);
}

bool Settings::runnerEnabled() {
    return q().value(QStringLiteral("runner/enabled"), true).toBool();
}

QString Settings::runnerPath() {
    const QString configured = q().value(QStringLiteral("runner/path")).toString();
    if (!configured.isEmpty()) return configured;
    return QCoreApplication::applicationDirPath() + QStringLiteral("/axc-runner");
}

bool Settings::debugOnRun() {
    return q().value(QStringLiteral("runner/debug"), false).toBool();
}

bool Settings::multiThreadDebug() {
    return q().value(QStringLiteral("debug/multiThread"), false).toBool();
}

alx::uint_32 Settings::runWallMs() {
    return static_cast<alx::uint_32>(q().value(QStringLiteral("runner/wallMs"), 0).toUInt());
}

alx::uint_64 Settings::runMemBytes() {
    return static_cast<alx::uint_64>(q().value(QStringLiteral("runner/memBytes"), 0).toULongLong());
}

alx::uint_64 Settings::runFrameMs() {
    return frame_number("runner/frameMs", proto::config::kFrameMsDefault, proto::config::kFrameMsMin,
                        proto::config::kFrameMsMax);
}

alx::uint_64 Settings::runFrameBytes() {
    return frame_number("runner/frameBytes", proto::config::kFrameBytesDefault,
                        proto::config::kFrameBytesMin, proto::config::kFrameBytesMax);
}

alx::uint_64 Settings::runTrimBytes() {
    return frame_number("runner/trimBytes", proto::config::kTrimBytesDefault, proto::config::kTrimBytesMin,
                        proto::config::kTrimBytesMax);
}

bool Settings::runLogFile() {
    return q().value(QStringLiteral("runner/logFile"), true).toBool();
}

void Settings::setRunnerEnabled(bool _on) {
    q().setValue(QStringLiteral("runner/enabled"), _on);
}

void Settings::setRunnerPath(const QString& _path) {
    q().setValue(QStringLiteral("runner/path"), _path);
}

void Settings::setRunWallMs(alx::uint_32 _ms) {
    q().setValue(QStringLiteral("runner/wallMs"), _ms);
}

void Settings::setRunMemBytes(alx::uint_64 _bytes) {
    q().setValue(QStringLiteral("runner/memBytes"), static_cast<qulonglong>(_bytes));
}

void Settings::setRunFrameMs(alx::uint_64 _ms) {
    q().setValue(QStringLiteral("runner/frameMs"), static_cast<qulonglong>(_ms));
}

void Settings::setRunFrameBytes(alx::uint_64 _bytes) {
    q().setValue(QStringLiteral("runner/frameBytes"), static_cast<qulonglong>(_bytes));
}

void Settings::setRunTrimBytes(alx::uint_64 _bytes) {
    q().setValue(QStringLiteral("runner/trimBytes"), static_cast<qulonglong>(_bytes));
}

void Settings::setRunLogFile(bool _on) {
    q().setValue(QStringLiteral("runner/logFile"), _on);
}

int Settings::logKeepDays() {
    return q().value(QStringLiteral("runner/logKeepDays"), 7).toInt();
}

void Settings::setLogKeepDays(int _days) {
    q().setValue(QStringLiteral("runner/logKeepDays"), _days);
}

void Settings::setDebugOnRun(bool _on) {
    q().setValue(QStringLiteral("runner/debug"), _on);
}

void Settings::setMultiThreadDebug(bool _on) {
    q().setValue(QStringLiteral("debug/multiThread"), _on);
}

const QList<ShortcutDef>& Settings::allShortcuts() {
    // function-local static: lazily built on the first call — QKeySequence
    // StandardKey construction needs a running QGuiApplication, so a namespace
    // static here would crash during static init
    static const QList<ShortcutDef> list = {
        {"file.new", "新建", QKeySequence::New},
        {"file.open", "打开", QKeySequence::Open},
        {"file.save", "保存", QKeySequence::Save},
        {"file.exit", "退出", QKeySequence::Quit},
        {"edit.undo", "撤销", QKeySequence::Undo},
        {"edit.redo", "重做", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z)},
        {"edit.cut", "剪切", QKeySequence::Cut},
        {"edit.copy", "复制", QKeySequence::Copy},
        {"edit.paste", "粘贴", QKeySequence::Paste},
        {"line.cut", "行剪切", QKeySequence::Cut},
        {"line.copy", "行复制", QKeySequence::Copy},
        {"line.paste", "行粘贴", QKeySequence::Paste},
        {"line.moveUp", "行上移", QKeySequence(Qt::ALT | Qt::Key_Up)},
        {"line.moveDown", "行下移", QKeySequence(Qt::ALT | Qt::Key_Down)},
        {"debug.run", "调试运行 / 继续", QKeySequence(Qt::Key_F5)},
        {"debug.runNoDebug", "运行（不调试）", QKeySequence(Qt::CTRL | Qt::Key_F5)},
        {"debug.rerun", "重新运行", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F5)},
        {"debug.stepOver", "步过", QKeySequence(Qt::Key_F10)},
        {"debug.stepInto", "步入", QKeySequence(Qt::Key_F11)},
        {"debug.stepOut", "步出", QKeySequence(Qt::SHIFT | Qt::Key_F11)},
        {"debug.runToCursor", "运行到光标", QKeySequence(Qt::CTRL | Qt::Key_F10)},
        {"debug.logpoint", "日志断点（当前行）", QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_L)},
        {"debug.condition", "条件断点（当前行）", QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_C)},
        {"console.open", "打开控制台", QKeySequence(Qt::CTRL | Qt::Key_QuoteLeft)},
        {"console.reset", "重置控制台", QKeySequence()},
        {"debug.interrupt", "中断", QKeySequence(Qt::Key_Escape)},
        {"view.ast", "查看 AST", QKeySequence(Qt::ALT | Qt::Key_A)},
        {"tool.compile", "编译", QKeySequence(Qt::CTRL | Qt::Key_B)},
        {"tool.format", "格式化", QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_F)},
    };
    return list;
}

QKeySequence Settings::shortcut(const QString& _id) {
    for (const auto& s : allShortcuts())
        if (_id == QLatin1String(s.id)) {
            const QString v = q().value(QStringLiteral("shortcut/") + _id).toString();
            return v.isEmpty() ? s.def : QKeySequence::fromString(v);
        }
    return {};
}

void Settings::resetShortcut(const QString& _id) {
    q().remove(QStringLiteral("shortcut/") + _id);
}
