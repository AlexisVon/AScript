#ifndef ASETTINGS_H
#define ASETTINGS_H

#include <QKeySequence>

#include <avariant.h>
#include <QList>
#include <QString>
#include <QStringList>

// Settings — QSettings-backed app configuration: editor/output fonts, shortcut
// table, completion parameters. Shortcuts live in one table (id → default →
// QSettings override) consumed by menus (QScriptMain) and the editor.
struct ShortcutDef {
    const char* id; // QSettings key: "shortcut/<id>"
    const char* name; // display name (Chinese, shown in the settings page)
    QKeySequence def;
};

class Settings {
public:
    // ── editor / output fonts ──
    static QString editorFont(); // family, "Fira Code" default
    static int editorFontSize(); // default 11
    static int outputFontSize(); // terminal + system log + debug panes, default 11
    static int uiScalePercent(); // whole-UI scale, 100/125/150/175/200; restart to apply
    static void setUiScalePercent(int _pct);

    // ── library dirs ──
    static QStringList userLibraryDirs(); // raw expressions: absolute / relative / {script} / {exe}
    static void setUserLibraryDirs(const QStringList& _dirs);
    static QStringList systemLibraryDirs(); // read-only, provided by the host

    // ── code completion ──
    static bool completionEnabled();
    static int completionDelay(); // popup debounce ms, default 300
    static int completionWindowItems(); // popup height in rows (window size), min 4, default 8
    static int completionListLimit(); // total candidate cap, 0 = unlimited, default 0
    static int completionCacheLimit(); // link/import doc entries, 0 = unlimited, default 64
    static void setCompletionCacheLimit(int _entries);
    static bool completionShowDesc();
    static int completionFontSize(); // default 11

    // ── hover tooltip ──
    static bool hoverEnabled();
    static int hoverFontSize(); // default 11

    // ── recent files/folders (dialog-opened only) ──
    static QStringList recentPaths(); // newest first, existing paths only, max 10
    static void addRecentPath(const QString& _path);

    // ── compile ──
    static bool compileDebug(); // position markers in the product (debug_enable)
    static bool compileEmbed(); // embed imports
    static void setCompileDebug(bool _on);
    static void setCompileEmbed(bool _on);

    // ── execution ──
    static bool runnerEnabled(); // execution on/off; false refuses to run anything
    static QString runnerPath(); // axc-runner binary; default <appdir>/axc-runner
    static bool debugOnRun(); // debug mode by default; until the breakpoint store drives it
    static bool multiThreadDebug(); // debug child engines created by $thd_create
    static alx::uint_32 runWallMs(); // watchdog budget per run; 0 = unlimited
    static alx::uint_64 runMemBytes(); // address-space cap per runner; 0 = unlimited
    static alx::uint_64 runFrameMs(); // display frame interval [ms]; 0 = no time framing
    static alx::uint_64 runFrameBytes(); // display frame size cap [bytes]; 0 = no cap
    static alx::uint_64 runTrimBytes(); // per-write display cap [bytes]; 0 = no trim
    static bool runLogFile(); // write the session log; true by default
    static void setRunnerEnabled(bool _on);
    static void setRunnerPath(const QString& _path);
    static void setRunWallMs(alx::uint_32 _ms);
    static void setRunMemBytes(alx::uint_64 _bytes);
    static void setRunFrameMs(alx::uint_64 _ms);
    static void setRunFrameBytes(alx::uint_64 _bytes);
    static void setRunTrimBytes(alx::uint_64 _bytes);
    static void setRunLogFile(bool _on);
    static void setDebugOnRun(bool _on);
    static void setMultiThreadDebug(bool _on);
    static int logKeepDays(); // session logs older than this are pruned; 0 = keep all
    static void setLogKeepDays(int _days);

    // ── shortcuts ──
    static const QList<ShortcutDef>& allShortcuts(); // full table with defaults
    static QKeySequence shortcut(const QString& _id); // QSettings override or default
    static void resetShortcut(const QString& _id); // drop the override
};

#endif // ASETTINGS_H
