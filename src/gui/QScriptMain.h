#ifndef QSCRIPTMAIN_H
#define QSCRIPTMAIN_H

#include <QHash>
#include <QMainWindow>
#include <QMap>

#include <functional>

QT_BEGIN_NAMESPACE
namespace Ui {
    class QScriptMain;
}
QT_END_NAMESPACE

class QAction;
class QCloseEvent;
class QShowEvent;
class LangRefDock;
class ViewDock;
class QToolButton;
class QTreeView;
class EditorArea;
class EmbedWidget;
class EngineObject;
class VariablesModel;
class FileTreeDock;
class HintProvider;
class OpenFilesPanel;
class QLabel;
class QPlainTextEdit;
class QScriptProject;
class QScriptSetting;
class QTreeWidget;
class QTreeWidgetItem;
class UiTerminal;
class PerfChart;
class PerfSampler;

class QScriptMain : public QMainWindow {
    Q_OBJECT

public:
    explicit QScriptMain(QWidget* parent = nullptr);
    ~QScriptMain() override;

    void openFile(const QString& _path);
    void openDir(const QString& _path); // shows the directory tree dock
    void openFileDialog();
    void openDirDialog();
    void openProject(const QString& _axPath); // .ax argument / 打开项目
    void openRecent(const QString& _path); // welcome list: file or folder
    void openAxpView(const QString& _path); // read-only .axp dock (T10)
    void newDocument();
    // setRunnerTracePath — record GUI↔runner protocol messages to a file (dev aid)
    void setRunnerTracePath(const QString& _path);

protected:
    void closeEvent(QCloseEvent* _e) override;
    void showEvent(QShowEvent* _e) override;
    bool eventFilter(QObject* _obj, QEvent* _ev) override;

private:
    void buildMenus();
    void buildDebugBar(); // floating debug control bar over the editor
    void showDebugBar(); // re-anchor top-center, then show
    void buildStatusBar();
    void buildHomeBar(); // rightmost vertical activity bar
    void showFileProps(const QString& _path); // EmbedWidget property popup
    void lockAxpView(const QString& _axpPath, const QString& _key, const QString& _alias); // T11: focus .axp dock on a module
    void openAxpTab(QTreeWidget* _tree, QTreeWidgetItem* _item); // open dump tab (double click / lock)
    void showAst(); // prtast dump into the output dock
    void about();
    void showLangRef();
    void runAx(const QString& _axPath, bool _debug); // .ax config-driven run
    void runCurrent(bool _debug);
    void runAgain();
    void runFile(const QString& _path, bool _debug); // one clicked file (per-file run exception)
    void leavePauseUi(); // pause exit shared by resume/step/run-to-cursor
    void step(int _kind); // 0 = over, 1 = into, 2 = out
    void runToCursor();
    void interruptSession(); // stop the live run; menu and debug bar share this path
    void fillModuleList(const QVariantList& _nodes);
    void fillThreadList(const QVariantList& _threads, quint64 _stop, int _known, int _captured);
    void fillFrameList(const QVariantList& _frames);
    void selectFrame(const QString& _ref); // frame ref → variable root, scope label
    void selectModule(const QString& _ref); // module ref → variable root, scope label
    void highlightModule(const QString& _source, const QString& _alias); // frame's owning module
    void setStepActionsEnabled(bool _on);
    void applyPauseState(bool _paused); // pause button: checked + play/pause icon + tooltip
    void resetSkipTrap();
    void openCompilePanel(const QString& _selectedFile, bool _treeFirst); // ax/axc compile dialog
    void formatCurrent(); // engine prtfmt on the current doc
    void logSys(const QString& _msg); // timestamped system log into the output dock
    void clearAllLogs(); // terminal + system log + session log files (the live one stays)
    void applySettingsFromConfig(); // settingsApplied — refresh fonts + action shortcuts
    void restoreLayout(); // window geometry only — docks wait for a file/folder
    void applyDockLayout(); // restore the saved dock layout once, on first open
    void saveLayout();
    bool scanAx(const QString& _dir); // activate the sole root .ax of the opened dir
    bool activateProject(const QString& _axPath, bool _openMain);
    void clearProject();
    void refreshHintContext(); // .ax path/docpath → HintProvider
    void closeProject(); // leave project mode, keep the folder open
    void closeFolder(); // close the folder too; open tabs stay
    void updateModeUi(); // window title + files dock title + status
    void updateWindowTitle();
    void updateDockTitle();
    QString modeText() const; // current mode as status-bar text
    void updateStatusBar(); // idle mode text (run state overrides it temporarily)
    void setStatusMessage(const QString& _msg); // transient status text (errors etc.)

    Ui::QScriptMain* ui;
    QLabel* m_statusLabel;
    EditorArea* m_editorArea;
    HintProvider* m_hints = nullptr;
    UiTerminal* m_terminal;
    EngineObject* m_worker;
    QScriptSetting* m_setting = nullptr;
    QScriptProject* m_axForm = nullptr; // .ax config form (edit on demand)
    QHash<QString, QAction*> m_actionById; // shortcut table id → menu action
    QAction* m_runAction;
    QAction* m_runNoDebugAction = nullptr;
    QAction* m_rerunAction = nullptr;
    QAction* m_closeProjectAction = nullptr;
    QAction* m_closeFolderAction = nullptr;
    QAction* m_logpointAction = nullptr;
    QAction* m_conditionAction = nullptr;
    bool m_hasLastRun = false;
    bool m_lastDebug = true;
    QAction* m_runToCursorAction = nullptr;
    QAction* m_openLogAction = nullptr;
    QAction* m_consoleAction = nullptr;
    QAction* m_consoleResetAction = nullptr;
    bool m_consoleActive = false;
    QList<QAction*> m_stepActions;
    QAction* m_interruptAction = nullptr;
    bool m_paused = false; // breakpoint hit, F5 resumes
    bool m_watching = false; // the view switch is on
    QString m_watchRef; // watched value ref
    QString m_watchType; // watched value type
    enum class scope { none,
                       frame,
                       module };
    scope m_scope = scope::none;
    QLabel* m_scopeLabel = nullptr; // debug pane: current variable scope
    QTreeView* m_debugVars = nullptr; // debug dock: lazy variable tree
    VariablesModel* m_varsModel = nullptr;
    ViewDock* m_viewDock = nullptr; // anyptr view dock (image page first)
    QTreeWidget* m_debugStack = nullptr; // call stack, innermost first
    QTreeWidget* m_debugTree = nullptr; // ent/link module tree
    QTreeWidget* m_threadTree = nullptr; // paused threads, main as the root
    QLabel* m_threadLabel = nullptr; // "N/M 已捕获"
    EmbedWidget* m_debugBar = nullptr; // floating debug controls (run only)
    int m_debugBarTop = 0; // auto-placed y; a dragged bar no longer matches it
    QToolButton* m_stopBtn = nullptr; // debug bar: stop button
    QToolButton* m_pauseBtn = nullptr; // debug bar: pause/resume toggle
    QToolButton* m_skipTrapBtn = nullptr; // debug bar: skip/enable trap toggle
    QPlainTextEdit* m_sysLog = nullptr; // system log pane (output dock tab)
    QDockWidget* m_perfDock = nullptr; // CPU/memory charts dock
    PerfChart* m_perfCpu = nullptr;
    PerfChart* m_perfMem = nullptr;
    PerfSampler* m_perf = nullptr;
    FileTreeDock* m_fileTree = nullptr;
    OpenFilesPanel* m_openFiles = nullptr;
    std::function<void()> m_fitOpenFiles; // open-files splitter fit (after layout)
    QDockWidget* m_langDock = nullptr;
    LangRefDock* m_langRef = nullptr;
    enum class WorkMode { None,
                          File,
                          Folder,
                          Project };
    WorkMode m_mode = WorkMode::None;
    bool m_dockLayoutApplied = false; // docks restored (and safe to overwrite on exit)
    QString m_dir; // opened folder (empty = single-file mode)
    QString m_projectName; // current project display name
    QString m_startupAx; // current project .ax (empty = none)
    QString m_runningAxp; // .axp currently run (breakpoint binding); cleared on finish
    QMap<QString, QDockWidget*> m_axpDocks; // .axp abs path → open read-only dock
    QLabel* m_posLabel = nullptr; // status bar cursor position (行/列)
};
#endif // QSCRIPTMAIN_H
