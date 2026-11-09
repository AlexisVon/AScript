#include "QScriptMain.h"
#include "ui_QScriptMain.h"

#include "BreakpointStore.h"
#include "CompilePanel.h"
#include "EditorArea.h"
#include "FileTreeDock.h"
#include "HintProvider.h"
#include "Icons.h"
#include "LangRefDock.h"
#include "OpenFilesPanel.h"
#include "PropsPanel.h"

#include <ascript.h>
#include "core/project.h"
#include "EmbedWidget.h"
#include "EngineObject.h"
#include "PerfChart.h"
#include "QScriptProject.h"
#include "QScriptSetting.h"
#include "ScriptEditor.h"
#include "Settings.h"
#include "debug/PerfSampler.h"
#include "debug/VariablesModel.h"
#include "UiTerminal.h"
#include "ViewDock.h"

#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QCloseEvent>
#include <QColor>
#include <QInputDialog>
#include <QDesktopServices>
#include <QDockWidget>
#include <QDir>
#include <QEvent>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QFile>
#include <QFileDialog>
#include <QGuiApplication>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFrame>
#include <QFontMetrics>
#include <QVariantList>
#include <QVariantMap>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPalette>
#include <QPlainTextEdit>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QTime>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include <memory>

namespace {

    // splitAxpKey — "path@sha16" embed reference (embed module inside a .axp):
    // "@" splits cleanly — path@sha is not a legal path. Returns the .axp path
    // and the engine module key ("@" + sha); false = plain file path.
    bool splitAxpKey(const QString& _file, QString& _axp, QString& _key) {
        const int at = _file.lastIndexOf('@');
        if (at <= 0) return false;
        const QString sha = _file.mid(at + 1);
        if (sha.size() != 16) return false;
        for (QChar c : sha)
            if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f')) return false;
        _axp = _file.left(at);
        _key = "@" + sha; // engine module key form
        return true;
    }

    // debug_bar_top — visible tab-bar height over the editor; 0 when no tabs
    int debug_bar_top(EditorArea* _area) {
        auto* tabBar = _area->findChild<QTabBar*>(QString(), Qt::FindDirectChildrenOnly);
        return tabBar != nullptr && tabBar->isVisible() ? tabBar->height() : 0;
    }

    // frameScopeText — top-level frames have no function name ("顶层")
    QString frameScopeText(const QString& _name, bool _topLevel) {
        return _topLevel ? QStringLiteral("作用域: 顶层")
                         : QStringLiteral("作用域: 帧 ") + _name;
    }

    // AxpDock — read-only .axp dock: closing is a real delete (not dock-hide)
    class AxpDock : public QDockWidget {
    public:
        using QDockWidget::QDockWidget;

    protected:
        void closeEvent(QCloseEvent* _e) override {
            _e->accept();
            deleteLater();
        }
    };

} // namespace

QScriptMain::QScriptMain(QWidget* parent)
    : QMainWindow(parent), ui(new Ui::QScriptMain) {
    ui->setupUi(this);
    setDockNestingEnabled(true);

    // Each dock is independent: reorderable by title bar drag (system title
    // bar kept), never floating. Collapse only via homebar toggle buttons.
    // reorderable by drag, closable (× = collapse), never floating
    for (QDockWidget* d : {ui->dockFiles, ui->dockOutput, ui->dockDebug})
        d->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
    // All docks hidden at startup: files only appears for a directory arg
    // (openDir), output/debug pop up on run. Uniform minimum sizes keep the
    // layout from jumping when they toggle.
    ui->dockFiles->hide();
    ui->dockDebug->hide();
    ui->dockOutput->hide();
    ui->dockFiles->setMinimumWidth(200);
    ui->dockDebug->setMinimumWidth(200);
    ui->dockOutput->setMinimumHeight(140);

    // directory tree panel (extracted to FileTreeDock) — dock content (open files + tree) is assembled below
    m_fileTree = new FileTreeDock;
    // forward file tree signals to main window orchestration
    connect(m_fileTree, &FileTreeDock::fileOpened, this, &QScriptMain::openFile);
    connect(m_fileTree, &FileTreeDock::axpSelected, this, &QScriptMain::openAxpView);
    connect(m_fileTree, &FileTreeDock::logMessage, this, &QScriptMain::logSys);
    connect(m_fileTree, &FileTreeDock::propsRequested, this, &QScriptMain::showFileProps);
    connect(m_fileTree, &FileTreeDock::axConfigRequested, this, [this](const QString& _path) {
        m_axForm->setAxPath(_path);
        m_axForm->move_center();
        m_axForm->show();
    });
    connect(m_fileTree, &FileTreeDock::compileRequested, this,
            [this](const QString& _path) { openCompilePanel(_path, false); });
    connect(m_fileTree, &FileTreeDock::compileTreeRequested, this,
            [this] { openCompilePanel(QString(), true); });
    connect(m_fileTree, &FileTreeDock::runRequested, this,
            [this](const QString& _path) { runFile(_path, false); });
    connect(m_fileTree, &FileTreeDock::runDebugRequested, this,
            [this](const QString& _path) { runFile(_path, true); });
    connect(m_fileTree, &FileTreeDock::axSelected, this, [this](const QString& _path) {
        if (QFileInfo(_path).suffix() == "ax") activateProject(_path, true);
    });

    // Sub-UI wiring: editor tabs (from .ui), terminal into output dock, engine worker
    // No untitled doc here — docs open on open/new only
    m_editorArea = ui->tabWidget; // promoted EditorArea in .ui
    m_hints = new HintProvider();
    m_hints->setLimit(Settings::completionCacheLimit());
    m_editorArea->setHintProvider(m_hints);

    // files dock: open-files list on top, file tree below (tree can be empty)
    m_openFiles = new OpenFilesPanel;
    auto* filesSplit = new QSplitter(Qt::Vertical);
    filesSplit->addWidget(m_openFiles);
    filesSplit->addWidget(m_fileTree);
    filesSplit->setStretchFactor(0, 0);
    filesSplit->setStretchFactor(1, 1);
    filesSplit->setSizes({96, 420});
    ui->dockFiles->setWidget(filesSplit);
    // open-files height follows its content until the user drags the splitter
    auto filesTouched = std::make_shared<bool>(false);
    connect(filesSplit, &QSplitter::splitterMoved, this,
            [filesTouched](int, int) { *filesTouched = true; });
    auto fitFiles = [this, filesSplit, filesTouched] {
        if (*filesTouched) return;
        const int total = filesSplit->height();
        if (total < 120) return;
        const int top = qBound(24, m_openFiles->preferredHeight(), total - 80);
        filesSplit->setSizes({top, total - top});
    };
    connect(ui->dockFiles, &QDockWidget::visibilityChanged, filesSplit,
            [fitFiles, filesSplit](bool _v) {
                if (_v) QTimer::singleShot(0, filesSplit, fitFiles);
            });
    m_fitOpenFiles = fitFiles; // re-run once the window is actually laid out
    connect(m_openFiles, &OpenFilesPanel::activateRequested, m_editorArea,
            &EditorArea::activateDocument);
    connect(m_openFiles, &OpenFilesPanel::runRequested, this,
            [this](const QString& _path) { runFile(_path, false); });
    connect(m_openFiles, &OpenFilesPanel::runDebugRequested, this,
            [this](const QString& _path) { runFile(_path, true); });
    connect(m_openFiles, &OpenFilesPanel::compileRequested, this,
            [this](const QString& _path) { openCompilePanel(_path, false); });
    connect(m_openFiles, &OpenFilesPanel::closeRequested, m_editorArea,
            &EditorArea::closeDocument);
    connect(m_editorArea, &EditorArea::documentsChanged, this, [this, fitFiles] {
        m_openFiles->setDocuments(m_editorArea->openDocuments());
        QTimer::singleShot(0, this, fitFiles);
        updateWindowTitle();
        if (!m_worker->isRunning() && !m_paused) updateStatusBar();
        if (m_debugBar != nullptr && m_debugBar->isVisible()) {
            const int top = debug_bar_top(m_editorArea) + 4;
            if (m_debugBar->y() == m_debugBarTop && top != m_debugBarTop)
                m_debugBar->move(m_debugBar->x(), top); // tab bar appeared/disappeared
            m_debugBarTop = top;
            m_debugBar->raise(); // the welcome page may have been raised above it
        }
    });
    m_openFiles->setDocuments(m_editorArea->openDocuments());

    m_terminal = new UiTerminal(ui->outputRunTab);
    auto* outLay = new QVBoxLayout(ui->outputRunTab);
    outLay->setContentsMargins(0, 0, 0, 0);
    outLay->addWidget(m_terminal);

    // system log pane: UI operations (new/move/delete/…) beside the engine output
    m_sysLog = new QPlainTextEdit;
    m_sysLog->setReadOnly(true);
    m_sysLog->setMaximumBlockCount(2000);
    m_sysLog->document()->setDocumentMargin(6);
    ui->outputTabs->addTab(m_sysLog, "系统");

    // debug dock panes: variables on top, call stack / modules below
    m_varsModel = new VariablesModel(this);
    m_scopeLabel = new QLabel(QStringLiteral("作用域: -"));
    m_scopeLabel->setObjectName(QStringLiteral("scopeLabel"));
    m_debugVars = new QTreeView;
    m_debugVars->setModel(m_varsModel);
    m_debugVars->setRootIsDecorated(true);
    m_debugVars->setAlternatingRowColors(true);
    m_debugVars->setUniformRowHeights(true);
    m_debugVars->header()->setStretchLastSection(false);
    m_debugVars->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_debugVars->header()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_debugVars->setColumnWidth(0, 120);
    m_debugVars->setColumnWidth(2, 40);
    auto* varsPane = new QWidget;
    auto* varsLay = new QVBoxLayout(varsPane);
    varsLay->setContentsMargins(0, 0, 0, 0);
    varsLay->setSpacing(2);
    varsLay->addWidget(m_scopeLabel);
    varsLay->addWidget(m_debugVars);

    m_debugStack = new QTreeWidget;
    m_debugStack->setColumnCount(2);
    m_debugStack->setHeaderLabels({QStringLiteral("函数"), QStringLiteral("位置")});
    m_debugStack->setRootIsDecorated(false);
    m_debugStack->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_debugStack->setSelectionMode(QAbstractItemView::SingleSelection);
    m_debugStack->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_debugStack->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_debugStack->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);

    m_debugTree = new QTreeWidget; // ent/link module tree
    m_debugTree->setHeaderHidden(true);
    m_debugTree->setEditTriggers(QAbstractItemView::NoEditTriggers);

    m_threadTree = new QTreeWidget;
    m_threadTree->setColumnCount(2);
    m_threadTree->setHeaderLabels({QStringLiteral("线程"), QStringLiteral("位置")});
    m_threadTree->setRootIsDecorated(true);
    m_threadTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_threadTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_threadTree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_threadTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_threadTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_threadLabel = new QLabel(QStringLiteral("线程: -"));
    m_threadLabel->setObjectName(QStringLiteral("threadLabel"));
    auto* threadPane = new QWidget;
    auto* threadLay = new QVBoxLayout(threadPane);
    threadLay->setContentsMargins(0, 0, 0, 0);
    threadLay->setSpacing(2);
    threadLay->addWidget(m_threadLabel);
    threadLay->addWidget(m_threadTree);

    auto* lowerRight = new QSplitter(Qt::Vertical);
    lowerRight->addWidget(m_debugTree);
    lowerRight->addWidget(threadPane);
    lowerRight->setStretchFactor(0, 1);
    lowerRight->setStretchFactor(1, 1);

    auto* lowerSplit = new QSplitter(Qt::Horizontal);
    lowerSplit->addWidget(m_debugStack);
    lowerSplit->addWidget(lowerRight);
    lowerSplit->setStretchFactor(0, 2);
    lowerSplit->setStretchFactor(1, 1);

    auto* debugSplit = new QSplitter(Qt::Vertical, ui->debugPanes);
    debugSplit->addWidget(varsPane);
    debugSplit->addWidget(lowerSplit);
    debugSplit->setStretchFactor(0, 3);
    debugSplit->setStretchFactor(1, 1);
    debugSplit->setSizes({300, 100});
    auto* debugLay = new QVBoxLayout(ui->debugPanes);
    debugLay->setContentsMargins(0, 0, 0, 0);
    debugLay->addWidget(debugSplit);

    m_viewDock = new ViewDock(this);
    addDockWidget(Qt::RightDockWidgetArea, m_viewDock);
    m_viewDock->hide();
    applySettingsFromConfig(); // output font size for log/debug panes

    m_worker = new EngineObject(this);
    connect(m_worker, &EngineObject::outputText, m_terminal, &UiTerminal::append);
    connect(m_worker, &EngineObject::logMessage, this, &QScriptMain::logSys);
    connect(m_worker, &EngineObject::logPathChanged, this, [this](const QString& _path) {
        m_terminal->setLogPath(_path);
        m_openLogAction->setEnabled(!_path.isEmpty());
    });
    connect(m_terminal, &UiTerminal::logPathClicked, this, [this] {
        const QString path = m_worker->logPath();
        if (!path.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });
    connect(m_varsModel, &VariablesModel::snapshotRequested, m_worker, &EngineObject::requestSnapshot);
    connect(m_varsModel, &VariablesModel::watchToggled, this,
            [this](const QString& _ref, const QString& _type, bool _on) {
                m_watching = _on;
                m_watchRef = _on ? _ref : QString();
                m_watchType = _on ? _type : QString();
                if (!_on) {
                    m_viewDock->clear_view();
                    m_viewDock->hide();
                    return;
                }
                m_viewDock->show();
                m_viewDock->raise();
                m_worker->requestView(_ref);
            });
    connect(m_worker, &EngineObject::viewReady, this,
            [this](const QString& _ref, bool _ok, const QString& _error, const QVariantMap& _view) {
                if (!m_watching || _ref != m_watchRef) return;
                if (_ok) m_viewDock->show_view(_ref, m_watchType, _view);
                else m_viewDock->show_failure(_ref, m_watchType, _error);
            });
    connect(m_worker, &EngineObject::consoleReady, this, [this] {
        m_consoleActive = true;
        m_terminal->setInputVisible(true);
        m_statusLabel->setText(QStringLiteral("控制台就绪"));
        m_interruptAction->setEnabled(true);
        m_runAction->setEnabled(false);
        m_runNoDebugAction->setEnabled(false);
        logSys(QStringLiteral("控制台已连接（长活进程）"));
    });
    connect(m_worker, &EngineObject::snapshotReady,
            [this](const QString& _ref, quint64 _ofst, const QVariantList& _nodes, bool _more) {
                if (_ref == QLatin1String("modules")) {
                    fillModuleList(_nodes);
                    return;
                }
                m_varsModel->apply(_ref, _ofst, _nodes, _more);
            });
    connect(m_worker, &EngineObject::inputRequested, this, [this] {
        m_terminal->append(">>> ");
        m_terminal->setInputVisible(true);
        m_statusLabel->setText("等待输入…");
    });
    connect(m_worker, &EngineObject::execResult, this, [this](const QString& _val, qint64 _us, bool _ok, int _err) {
        // format elapsed time
        double ms = _us / 1000.0;
        QString elapsed = ms < 1000
                              ? QString::number(ms, 'f', 1) + " ms"
                              : QString::number(ms / 1000.0, 'f', 2) + " s";

        if (_ok) {
            // show return value (skip null — engine's default)
            if (_val != "?")
                m_terminal->append("\n= " + _val + "\n");
            m_terminal->append("[elapsed " + elapsed + "]\n");
        } else {
            // ensure newline before error (engine on_cerr may lack trailing newline)
            m_terminal->append("\n[elapsed " + elapsed + "] " + alx::script::error_type_name(static_cast<alx::script::error_type>(_err)) + "\n");
        }
    });
    connect(m_worker, &EngineObject::finished, this, [this](bool _ok, const QString& _msg) {
        logSys(_ok ? "运行完成" : "运行失败: " + _msg);
        if (!m_worker->logPath().isEmpty()) logSys(QStringLiteral("会话日志: ") + m_worker->logPath());
        m_paused = false;
        m_consoleActive = false;
        if (m_perf) m_perf->stop();
        m_runAction->setText("调试运行(&F5)");
        m_interruptAction->setEnabled(false);
        m_stopBtn->setEnabled(false);
        m_pauseBtn->setEnabled(false);
        applyPauseState(false);
        m_skipTrapBtn->setEnabled(false);
        m_debugBar->hide();
        resetSkipTrap();
        if (_ok) updateStatusBar(); // back to the startup-project hint
        else m_statusLabel->setText("运行失败");
        if (!_msg.isEmpty()) m_terminal->append(_msg + "\n");
        m_runAction->setEnabled(true);
        m_runNoDebugAction->setEnabled(true);
        m_rerunAction->setEnabled(m_hasLastRun);
        m_openLogAction->setEnabled(!m_worker->logPath().isEmpty());
        m_runToCursorAction->setEnabled(false);
        setStepActionsEnabled(false);
        m_varsModel->clear();
        m_watching = false;
        m_watchRef.clear();
        m_watchType.clear();
        if (m_viewDock) {
            m_viewDock->clear_view();
            m_viewDock->hide();
        }
        m_debugStack->clear();
        m_debugTree->clear();
        m_threadTree->clear();
        m_threadLabel->setText(QStringLiteral("线程: -"));
        m_scope = scope::none;
        m_scopeLabel->setText(QStringLiteral("作用域: -"));
        m_runningAxp.clear();
        m_editorArea->clearExecutionLines();
    });
    connect(m_worker, &EngineObject::paused, this, [this] {
        m_paused = true;
        m_runAction->setEnabled(true);
        m_runAction->setText("继续(&F5)");
        m_interruptAction->setEnabled(true);
        applyPauseState(true);
        m_statusLabel->setText("暂停中…");
        setStepActionsEnabled(false); // focusChanged enables them for a main-thread focus
        m_runToCursorAction->setEnabled(false);
    });
    // focusChanged — the stack, variables, modules and view follow the focused thread
    connect(m_worker, &EngineObject::focusChanged, this, [this] {
        const bool mainThread = m_worker->focusThread() == 0;
        setStepActionsEnabled(mainThread);
        m_runToCursorAction->setEnabled(mainThread && !m_editorArea->currentPath().isEmpty());
        const QString ref = m_worker->snapshotRef();
        if (!ref.isEmpty()) {
            m_varsModel->setRoot(ref, false); // Session already asked for the first page
            m_scope = scope::frame;
        }
        m_worker->requestSnapshot(QStringLiteral("modules"));
        if (m_watching && !m_watchRef.isEmpty()) m_worker->requestView(m_watchRef);
    });
    connect(m_worker, &EngineObject::threadsChanged, this,
            [this](const QVariantList& _threads, quint64 _stop, int _known, int _captured) {
                fillThreadList(_threads, _stop, _known, _captured);
            });
    connect(m_worker, &EngineObject::trapReported, this,
            [this](const QString& _label, const QString& _args) {
                QString text = QStringLiteral("[trap]");
                if (!_label.isEmpty()) text += QLatin1Char(' ') + _label;
                if (!_args.isEmpty()) text += QStringLiteral(": ") + _args;
                logSys(text);
                m_statusLabel->setText(text);
            });
    connect(m_threadTree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* _item, int) {
        if (_item) m_worker->setFocusThread(_item->data(0, Qt::UserRole).toULongLong());
    });
    // .axp binding: lock the read-only .axp dock to the frame's module (T11).
    // Embed frames carry "path@key" (module key matches the .axp modules map);
    // root frames fall back to the [::] view. Non-axp runs ignore the binding.
    connect(m_worker, &EngineObject::breakpointFrames, this, [this](const QVariantList& _frames) {
        fillFrameList(_frames);
        if (!_frames.isEmpty()) {
            QTreeWidgetItem* top = m_debugStack->topLevelItem(0);
            m_debugStack->setCurrentItem(top);
            m_scope = scope::frame;
            m_scopeLabel->setText(
                frameScopeText(top->text(0), top->data(0, Qt::UserRole + 4).toBool()));
            highlightModule(top->data(0, Qt::UserRole + 1).toString(),
                            top->data(0, Qt::UserRole + 2).toString());
        }
        if (m_runningAxp.isEmpty()) return;
        for (const auto& fv : _frames) {
            const QVariantMap f = fv.toMap();
            QString axp, key;
            if (splitAxpKey(f.value("file").toString(), axp, key)) { // path@sha — embed module
                lockAxpView(axp, key, f.value("alias").toString());
                return;
            }
        }
        lockAxpView(m_runningAxp, "@::", QString()); // top-level [::] frame
    });
    connect(m_debugStack, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* _item, int) {
        if (_item) selectFrame(_item->data(0, Qt::UserRole).toString());
    });
    // module tree single click: variables follow the module (links have no ref)
    connect(m_debugTree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* _item, int) {
        if (_item) selectModule(_item->data(0, Qt::UserRole + 1).toString());
    });
    // module tree double click: open the node's file — .axc in the editor,
    // .axp as a read-only dock, "path@key" embed module inside its .axp view
    connect(m_debugTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* _item, int) {
        const QString file = _item->data(0, Qt::UserRole).toString();
        if (file.isEmpty()) return;
        QString axp, key;
        if (splitAxpKey(file, axp, key)) { // path@sha — embed module inside its .axp
            lockAxpView(axp, key, _item->data(0, Qt::UserRole + 1).toString());
        } else if (file.endsWith(".axp")) {
            openAxpView(file);
        } else {
            openFile(file);
        }
    });
    connect(m_worker, &EngineObject::breakpointHit, this, [this](const QString& _file, int _row, int) {
        // the editor follows the main thread; a child's source stays out of it
        if (m_worker->focusThread() != 0) {
            m_editorArea->clearExecutionLines();
            return;
        }
        m_statusLabel->setText(QString("暂停于 行 %1").arg(_row));
        // .axp run: breakpointFrames already locked the .axp dock to the module;
        // skip openFile (the .axp binary is not a source file)
        if (!m_runningAxp.isEmpty()) return;
        // breakpoint in an imported file: open it (reuse tab) and switch to it
        ScriptEditor* ed = m_editorArea->currentEditor();
        if (!_file.isEmpty()) {
            const QString cur = m_editorArea->currentPath();
            if (QFileInfo(cur).canonicalFilePath() != QFileInfo(_file).canonicalFilePath())
                if (ScriptEditor* e = m_editorArea->openFile(_file)) ed = e;
        }
        if (ed) {
            ed->setExecutionLine(_row);
            ed->gotoLine(_row);
        }
    });
    connect(m_terminal, &UiTerminal::inputEntered, this, [this](const QString& _line) {
        if (m_consoleActive) {
            m_terminal->append(">>> " + _line + "\n");
            if (!m_worker->consoleExec(_line)) logSys(QStringLiteral("控制台拒绝该行"));
            return;
        }
        m_terminal->append(_line + "\n");
        m_worker->submitInput(_line);
        m_statusLabel->setText("运行中…");
    });
    connect(m_editorArea, &EditorArea::breakpointToggled, this, [this](const QString& _path, int _row) {
        if (_path.isEmpty()) {
            logSys(QStringLiteral("未命名文档暂不支持断点，请先保存文件"));
            return;
        }
        const bool on = BreakpointStore::instance().toggle(_path, _row);
        if (ScriptEditor* ed = m_editorArea->currentEditor())
            ed->setBreakpoints(BreakpointStore::instance().lines(_path));
        m_worker->syncBreakpoints(_path);
        logSys(QStringLiteral("%1断点 %2:%3").arg(on ? QStringLiteral("设置") : QStringLiteral("删除"), QFileInfo(_path).fileName()).arg(_row));
    });
    connect(m_editorArea, &EditorArea::runRequested, this, [this] { runCurrent(false); });
    connect(m_editorArea, &EditorArea::astRequested, this, &QScriptMain::showAst);
    connect(m_editorArea, &EditorArea::formatRequested, this, &QScriptMain::formatCurrent);
    connect(m_editorArea, &EditorArea::newRequested, this, &QScriptMain::newDocument);
    connect(m_editorArea, &EditorArea::openFileRequested, this, &QScriptMain::openFileDialog);
    connect(m_editorArea, &EditorArea::openDirRequested, this, &QScriptMain::openDirDialog);
    connect(m_editorArea, &EditorArea::recentRequested, this, &QScriptMain::openRecent);
    // editor ↔ file tree sync: tab switch → select in tree + raise .axp dock + select .axp sub-item
    connect(m_editorArea, &EditorArea::currentFileChanged, this, [this](const QString& _path) {
        if (m_dir.isEmpty() && !_path.isEmpty() && m_mode == WorkMode::None)
            m_mode = WorkMode::File;
        updateWindowTitle();
        if (!m_worker->isRunning() && !m_paused) updateStatusBar();
        if (ScriptEditor* ed = m_editorArea->currentEditor())
            ed->setBreakpoints(BreakpointStore::instance().lines(_path));
        m_fileTree->selectFile(_path);
        // clear selection in all .axp docks; re-select below if current tab is .axp
        for (auto it = m_axpDocks.constBegin(); it != m_axpDocks.constEnd(); ++it) {
            if (auto* t = qobject_cast<QTreeWidget*>(it.value()->widget()))
                t->clearSelection();
        }
        if (QFileInfo(_path).suffix() != "axp") return;
        const QString abs = QFileInfo(_path).absoluteFilePath();
        const int at = abs.lastIndexOf('@');
        const QString axpFile = at > 0 ? abs.left(at) : abs;
        auto* dock = m_axpDocks.value(axpFile);
        if (!dock) return;
        dock->raise();
        // select the matching item in the .axp nav tree
        auto* tree = qobject_cast<QTreeWidget*>(dock->widget());
        if (!tree) return;
        QTreeWidgetItem* root = tree->topLevelItem(0);
        if (!root) return;
        const QString title = m_editorArea->currentTabTitle();
        const QString suffix = title.contains(": ") ? title.section(": ", 1) : QString();
        QTreeWidgetItem* target = nullptr;
        for (int i = 0; i < root->childCount() && !target; ++i) {
            QTreeWidgetItem* c = root->child(i);
            if (suffix == "[::]" && c->text(0) == "[::]")
                target = c;
            else if (suffix.startsWith("modules/")) {
                const QString key = suffix.section('/', 1);
                for (int j = 0; j < c->childCount() && !target; ++j)
                    if (c->child(j)->data(0, Qt::UserRole + 1).toString() == key)
                        target = c->child(j);
            } else if (suffix == c->text(0))
                target = c;
        }
        if (target) {
            tree->setCurrentItem(target);
            tree->scrollToItem(target);
        }
    });

    m_setting = new QScriptSetting(this);
    connect(m_setting, &QScriptSetting::settingsApplied, this, [this] {
        applySettingsFromConfig();
        if (m_hints) {
            m_hints->setLimit(Settings::completionCacheLimit());
            refreshHintContext();
        }
    });

    m_axForm = new QScriptProject(this);
    connect(m_axForm, &QScriptProject::saved, this, [this](const QString& _path) {
        activateProject(_path, false);
        logSys("已保存 .ax 配置: " + _path);
        setStatusMessage("已保存 .ax 配置: " + QFileInfo(_path).fileName());
    });

    // status bar: wrap-aware cursor position of the current editor
    connect(m_editorArea, &EditorArea::cursorPosChanged, this, [this](int _row, int _col) {
        m_posLabel->setText(QString("行 %1, 列 %2").arg(_row).arg(_col));
    });

    // language reference dock — chapter tree + generated function manual, built on first show
    m_langDock = new QDockWidget(tr("语言参考"), this);
    m_langDock->setObjectName(QStringLiteral("dockLang"));
    m_langDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
    m_langDock->setMinimumWidth(300);
    m_langRef = new LangRefDock;
    m_langRef->setHintProvider(m_hints);
    m_langDock->setWidget(m_langRef);
    addDockWidget(Qt::RightDockWidgetArea, m_langDock);
    m_langDock->hide();

    // performance dock — CPU and memory of the runner process, sampled by pid
    m_perfDock = new QDockWidget(tr("性能"), this);
    m_perfDock->setObjectName(QStringLiteral("dockPerf"));
    m_perfDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
    m_perfDock->setMinimumWidth(220);
    auto* perfPane = new QWidget;
    auto* perfLay = new QVBoxLayout(perfPane);
    perfLay->setContentsMargins(4, 4, 4, 4);
    perfLay->setSpacing(4);
    m_perfCpu = new PerfChart(QStringLiteral("CPU"), true, QColor(0x2a, 0x78, 0xd6),
                              QColor(0x39, 0x87, 0xe5));
    m_perfMem = new PerfChart(QStringLiteral("内存"), false, QColor(0xeb, 0x68, 0x34),
                              QColor(0xd9, 0x59, 0x26));
    perfLay->addWidget(m_perfCpu);
    perfLay->addWidget(m_perfMem);
    perfLay->addStretch();
    m_perfDock->setWidget(perfPane);
    addDockWidget(Qt::RightDockWidgetArea, m_perfDock);
    m_perfDock->hide();

    m_perf = new PerfSampler(this);
    connect(m_perf, &PerfSampler::sampled, this, [this](double _cpu, quint64 _rss) {
        m_perfCpu->add(_cpu);
        m_perfMem->add(_rss);
    });
    connect(m_worker, &EngineObject::runnerStarted, this, [this](quint64 _pid) {
        m_perfCpu->reset();
        m_perfMem->reset();
        m_perfMem->setLimit(static_cast<double>(Settings::runMemBytes()));
        m_perf->start(_pid);
    });

    buildMenus();
    buildDebugBar();
    buildStatusBar();
    buildHomeBar();
    restoreLayout();
    updateModeUi();
}

QScriptMain::~QScriptMain() {
    delete m_worker; // unparented: not owned by the QObject tree
    delete m_hints; // unparented: editors hold a bare pointer, use ends before teardown
    // delete .axp docks here — the Qt tree would drop them during ~QObject,
    // after m_axpDocks is already gone (destroyed lambda would hit freed memory)
    const auto docks = std::move(m_axpDocks); // empty the map: the destroyed
    for (QDockWidget* d : docks) delete d; // lambda removes keys as they die

    m_debugBar = nullptr; // dies with the editor — keep the resize filter off it
    delete ui;
}

void QScriptMain::closeEvent(QCloseEvent* _e) {
    saveLayout();
    QMainWindow::closeEvent(_e);
}

void QScriptMain::showEvent(QShowEvent* _e) {
    QMainWindow::showEvent(_e);
    if (m_fitOpenFiles) QTimer::singleShot(0, this, m_fitOpenFiles);
}

bool QScriptMain::eventFilter(QObject* _obj, QEvent* _ev) {
    // keep the floating debug bar inside the editor when the window resizes
    if (_obj == m_editorArea && _ev->type() == QEvent::Resize && m_debugBar != nullptr &&
        m_debugBar->isVisible())
        m_debugBar->set_rect(m_editorArea->rect(), true); // clamps the current position
    return QMainWindow::eventFilter(_obj, _ev);
}

void QScriptMain::openFile(const QString& _path) {
    applyDockLayout();
    if (!m_editorArea->openFile(_path)) {
        m_statusLabel->setText("无法打开 " + _path);
        logSys("打开失败: " + _path);
    } else {
        logSys("打开: " + _path);
        if (m_dir.isEmpty()) m_mode = WorkMode::File;
        updateWindowTitle();
        if (!m_worker->isRunning() && !m_paused) updateStatusBar();
    }
}

void QScriptMain::newDocument() {
    applyDockLayout();
    m_editorArea->newDocument();
    if (m_dir.isEmpty()) m_mode = WorkMode::File;
    updateWindowTitle();
    if (!m_worker->isRunning() && !m_paused) updateStatusBar();
    logSys("新建文档");
}

void QScriptMain::logSys(const QString& _msg) {
    if (!m_sysLog) return;
    m_sysLog->appendPlainText(QTime::currentTime().toString("hh:mm:ss") + "  " + _msg);
}

void QScriptMain::setRunnerTracePath(const QString& _path) {
    m_worker->setRunnerTracePath(_path);
}

void QScriptMain::clearAllLogs() {
    const auto answer = QMessageBox::question(this, QStringLiteral("清空所有日志"),
                                              QStringLiteral("清空终端与系统日志，并删除历史会话日志文件？"),
                                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;
    m_terminal->clear();
    if (m_sysLog) m_sysLog->clear();
    const QString active = m_worker->logPath();
    const QDateTime now = QDateTime::currentDateTime();
    const QFileInfoList logs =
        QDir(QDir::tempPath()).entryInfoList({QStringLiteral("axc-session-*.log")}, QDir::Files);
    for (const auto& fi : logs) {
        if (!active.isEmpty() && fi.absoluteFilePath() == active) continue;
        if (fi.lastModified().secsTo(now) < 5) continue; // another instance may still be writing
        QFile::remove(fi.absoluteFilePath());
    }
    logSys(active.isEmpty() ? QStringLiteral("已清空所有日志")
                            : QStringLiteral("已清空所有日志（当前会话日志保留: %1）").arg(active));
}

void QScriptMain::applySettingsFromConfig() {
    // output panes: one shared font size (family stays the fixed system font)
    QFont outFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    outFont.setPointSize(Settings::outputFontSize());
    m_sysLog->setFont(outFont);
    m_debugVars->setFont(outFont);
    m_debugStack->setFont(outFont);
    m_threadTree->setFont(outFont);
    m_terminal->applySettings();
    if (m_perfMem) m_perfMem->setLimit(static_cast<double>(Settings::runMemBytes()));
    if (m_langRef) m_langRef->applySettings();
    // editors: font + completion params for every open doc
    m_editorArea->applySettings();
    // action shortcuts: rebind from the table (already registered in buildMenus)
    for (auto it = m_actionById.cbegin(); it != m_actionById.cend(); ++it)
        it.value()->setShortcut(Settings::shortcut(it.key()));
}

void QScriptMain::restoreLayout() {
    QSettings s(QStringLiteral("AScript"), QStringLiteral("AScript"));
    const QByteArray geo = s.value(QStringLiteral("window/geometry")).toByteArray();
    if (!geo.isEmpty()) restoreGeometry(geo);
}

void QScriptMain::applyDockLayout() {
    if (m_dockLayoutApplied) return;
    m_dockLayoutApplied = true;
    QSettings s(QStringLiteral("AScript"), QStringLiteral("AScript"));
    const QByteArray state = s.value(QStringLiteral("window/state")).toByteArray();
    if (!state.isEmpty()) restoreState(state);
}

void QScriptMain::saveLayout() {
    QSettings s(QStringLiteral("AScript"), QStringLiteral("AScript"));
    s.setValue(QStringLiteral("window/geometry"), saveGeometry());
    // welcome-page exits must not overwrite the last real dock layout
    if (m_dockLayoutApplied) s.setValue(QStringLiteral("window/state"), saveState());
}

void QScriptMain::openFileDialog() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("打开脚本"), QString(),
        QStringLiteral("AScript (*.axc *.ax *.axp *.json *.txt *.md);;所有文件 (*)"));
    if (path.isEmpty()) return;
    Settings::addRecentPath(QFileInfo(path).absoluteFilePath()); // dialogs feed recents
    const QString suffix = QFileInfo(path).suffix();
    if (suffix == "ax") openProject(path);
    else if (suffix == "axp") openAxpView(path);
    else openFile(path);
}

void QScriptMain::openDirDialog() {
    const QString path = QFileDialog::getExistingDirectory(this, QStringLiteral("打开目录"));
    if (path.isEmpty()) return;
    Settings::addRecentPath(QFileInfo(path).absoluteFilePath());
    openDir(path);
}

void QScriptMain::openProject(const QString& _axPath) {
    applyDockLayout();
    const QString abs = QFileInfo(_axPath).absoluteFilePath();
    if (!QFileInfo(abs).isFile()) {
        setStatusMessage("项目不存在: " + abs);
        return;
    }
    const QString dir = QFileInfo(abs).absolutePath();
    if (m_dir != dir) openDir(dir); // roots the tree; may already activate a lone .ax
    if (m_startupAx.isEmpty() || QFileInfo(m_startupAx).absoluteFilePath() != abs)
        activateProject(abs, true);
}

void QScriptMain::openRecent(const QString& _path) {
    const QFileInfo fi(_path);
    if (!fi.exists()) {
        setStatusMessage("路径不存在: " + _path);
        return;
    }
    if (fi.isDir()) {
        openDir(_path);
    } else if (fi.suffix() == "ax") {
        openProject(_path);
    } else if (fi.suffix() == "axp") {
        openAxpView(_path);
    } else {
        openFile(_path);
    }
}

void QScriptMain::openDir(const QString& _path) {
    applyDockLayout();
    m_dir = QFileInfo(_path).absoluteFilePath();
    m_mode = WorkMode::Folder;
    clearProject();
    m_fileTree->setRootPath(m_dir);
    updateModeUi(); // folder baseline; a project or parse error may replace it
    scanAx(m_dir); // may switch to Project and open main
    refreshHintContext(); // folder context when no project was activated
    const QString cur = m_editorArea->currentPath();
    if (!cur.isEmpty()) m_fileTree->selectFile(cur);
    ui->dockFiles->setVisible(true);
}

// openAxpView — read-only structure dock for a compiled .axp (T10): unpack via
// engine::unpack, show main AST / hint / modules / info as a tree. Already open
// → raise. Close = real delete (AxpDock::closeEvent).
void QScriptMain::openAxpView(const QString& _path) {
    applyDockLayout();
    const QString abs = QFileInfo(_path).absoluteFilePath();
    if (QDockWidget* d = m_axpDocks.value(abs)) {
        d->raise();
        d->activateWindow();
        return;
    }
    core::project::axp_view v;
    std::string err;
    if (!core::project::unpack_axp(abs.toStdString(), v, err)) {
        const QString msg = QString::fromStdString(err);
        setStatusMessage("解包失败: " + msg);
        logSys("解包失败: " + msg);
        return;
    }
    auto* dock = new AxpDock(QFileInfo(abs).fileName(), this);
    dock->setObjectName("axp:" + abs);
    dock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable);
    auto* tree = new QTreeWidget(dock);
    tree->setHeaderHidden(true);
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers); // read-only
    dock->setWidget(tree);

    // root node: relative to the opened dir; standalone → absolute, middle-elided
    const QString lastDir = m_fileTree ? m_fileTree->rootPath() : QString();
    QString rootLabel = lastDir.isEmpty() ? abs : QDir(lastDir).relativeFilePath(abs);
    if (lastDir.isEmpty())
        rootLabel = QFontMetrics(tree->font()).elidedText(rootLabel, Qt::ElideMiddle, 240);
    auto* root = new QTreeWidgetItem(tree);
    root->setText(0, rootLabel);
    root->setToolTip(0, abs);
    root->setExpanded(true);

    // navigation-only tree: each entry opens its raw text in a read-only
    // editor tab on double click (view type in UserRole, module key in UserRole+1).
    // "[::]" is the engine's program-root key (compile inner map); module keys
    // are engine-generated import ids (@hash)
    enum AxpView { V_Main = 0,
                   V_Hint,
                   V_Module,
                   V_Info };
    auto* mainItem = new QTreeWidgetItem(root);
    mainItem->setText(0, "[::]");
    mainItem->setData(0, Qt::UserRole, V_Main);

    if (v.has_hint) {
        auto* hintItem = new QTreeWidgetItem(root);
        hintItem->setText(0, "hint");
        hintItem->setData(0, Qt::UserRole, V_Hint);
    }
    if (!v.modules.empty()) {
        auto* modsItem = new QTreeWidgetItem(root); // directory node — no view
        modsItem->setText(0, QString("modules (%1)").arg(v.modules.size()));
        modsItem->setData(0, Qt::UserRole, -1); // marker: not a viewable item
        for (const auto& kv : v.modules) {
            auto* mItem = new QTreeWidgetItem(modsItem);
            mItem->setText(0, QString::fromStdString(kv.first));
            mItem->setData(0, Qt::UserRole, V_Module);
            mItem->setData(0, Qt::UserRole + 1, QString::fromStdString(kv.first));
        }
    }
    // info is a nav node too — contents live in the view, no child leaves
    auto* infoItem = new QTreeWidgetItem(root);
    infoItem->setText(0, "info");
    infoItem->setData(0, Qt::UserRole, V_Info);
    // dump texts cached on the dock — shared by double click and T11 lockAxpView
    QVariantMap texts;
    texts["main"] = QString::fromStdString(v.main_text);
    texts["info"] = QString::fromStdString(v.info_text);
    if (v.has_hint) texts["hint"] = QString::fromStdString(v.hint_text);
    for (const auto& kv : v.modules)
        texts["mod:" + QString::fromStdString(kv.first)] = QString::fromStdString(kv.second);
    dock->setProperty("axpTexts", texts);
    connect(tree, &QTreeWidget::itemDoubleClicked, this,
            [this, tree](QTreeWidgetItem* _item, int) { openAxpTab(tree, _item); });

    m_axpDocks[abs] = dock;
    connect(dock, &QObject::destroyed, this, [this, abs](QObject*) { m_axpDocks.remove(abs); });
    addDockWidget(Qt::RightDockWidgetArea, dock);
    logSys("解包: " + abs);
}

// openAxpTab — open the dump tab for an .axp nav item (double click / T11 lock).
// Texts come from the dock's cached axpTexts property (single unpack in openAxpView).
void QScriptMain::openAxpTab(QTreeWidget* _tree, QTreeWidgetItem* _item) {
    if (!_item) return;
    auto* dock = qobject_cast<QDockWidget*>(_tree->parentWidget()); // QTreeWidget hangs off the dock
    const QVariantMap texts = dock ? dock->property("axpTexts").toMap() : QVariantMap();
    QString title, text;
    EditorArea::ViewKind kind = EditorArea::Plain;
    switch (_item->data(0, Qt::UserRole).toInt()) {
    case 0: // V_Main
        title = "[::]";
        text = texts.value("main").toString();
        kind = EditorArea::Ast;
        break;
    case 1: // V_Hint
        title = "hint";
        text = texts.value("hint").toString();
        kind = EditorArea::Json;
        break;
    case 2: { // V_Module
        const QString key = _item->data(0, Qt::UserRole + 1).toString();
        title = "modules/" + key;
        text = texts.value("mod:" + key).toString();
        kind = EditorArea::Ast;
        break;
    }
    case 3: // V_Info
        title = "info";
        text = texts.value("info").toString();
        kind = EditorArea::Json;
        break;
    default:
        return;
    }
    if (text.isEmpty()) return;
    const QString axpPath = dock->objectName().mid(4);
    m_editorArea->openReadonly(
        QFileInfo(axpPath).fileName() + ": " + title, text, kind, axpPath);
}

// lockAxpView — T11: after a breakpoint in a .axp run, focus the read-only .axp
// dock on the frame's module: "@::" → the program root, "@hash" → modules/@hash.
void QScriptMain::lockAxpView(const QString& _axpPath, const QString& _key, const QString& _alias) {
    const QString abs = QFileInfo(_axpPath).absoluteFilePath();
    openAxpView(abs); // open or raise
    QDockWidget* dock = m_axpDocks.value(abs);
    auto* tree = dock ? qobject_cast<QTreeWidget*>(dock->widget()) : nullptr;
    QTreeWidgetItem* root = tree ? tree->topLevelItem(0) : nullptr;
    if (!root) return;
    QTreeWidgetItem* target = nullptr;
    for (int i = 0; i < root->childCount() && !target; i++) {
        QTreeWidgetItem* c = root->child(i);
        if (_key == "@::") {
            if (c->text(0) == "[::]") target = c;
        } else if (c->text(0).startsWith("modules")) {
            for (int j = 0; j < c->childCount() && !target; j++)
                if (c->child(j)->data(0, Qt::UserRole + 1).toString() == _key) target = c->child(j);
        }
    }
    if (!target) return;
    tree->setCurrentItem(target);
    tree->scrollToItem(target);
    tree->setFocus();
    openAxpTab(tree, target); // lock = open the module dump
    logSys(QString("断点锁定: %1%2").arg(_alias.isEmpty() ? QString() : _alias + " ", target->text(0).startsWith("[::]") ? "[::]" : _key));
}

// scanAx — a folder root with exactly one .ax becomes its project
bool QScriptMain::scanAx(const QString& _dir) {
    const QStringList axs = QDir(_dir).entryList(QStringList() << "*.ax", QDir::Files, QDir::Name);
    if (axs.size() != 1) return false;
    return activateProject(QDir(_dir).filePath(axs[0]), true);
}

bool QScriptMain::activateProject(const QString& _axPath, bool _openMain) {
    core::project::ax_config ax;
    std::string err;
    if (!core::project::parse_ax(_axPath.toStdString(), ax, err)) {
        logSys("工程配置解析失败: " + QString::fromStdString(err));
        setStatusMessage("工程配置解析失败: " + QFileInfo(_axPath).fileName());
        return false;
    }
    m_startupAx = QFileInfo(_axPath).absoluteFilePath();
    m_projectName = ax.name.empty() ? QFileInfo(m_startupAx).completeBaseName()
                                    : QString::fromStdString(ax.name);
    m_mode = WorkMode::Project;
    if (m_dir.isEmpty()) { // manual activation without an opened folder
        m_dir = QFileInfo(m_startupAx).absolutePath();
        m_fileTree->setRootPath(m_dir);
    }
    updateModeUi();
    refreshHintContext();
    logSys("工程: " + m_projectName + "  (" + m_startupAx + ")");
    if (_openMain) openFile(QString::fromStdString(ax.main));
    return true;
}

void QScriptMain::clearProject() {
    m_startupAx.clear();
    m_projectName.clear();
    if (m_mode == WorkMode::Project) m_mode = WorkMode::Folder;
    refreshHintContext();
}

void QScriptMain::refreshHintContext() {
    if (!m_hints) return;
    m_hints->clear();
    if (m_startupAx.isEmpty()) {
        m_hints->setProject(m_dir, {}, {}, Settings::userLibraryDirs());
        if (m_langRef) m_langRef->refreshLibraries();
        return;
    }
    core::project::ax_config ax;
    std::string err;
    if (!core::project::parse_ax(m_startupAx.toStdString(), ax, err)) {
        m_hints->setProject(m_dir, {}, {}, Settings::userLibraryDirs());
        if (m_langRef) m_langRef->refreshLibraries();
        return;
    }
    QStringList path, docpath;
    for (const std::string& p : ax.path) path << QString::fromStdString(p);
    for (const std::string& p : ax.docpath) docpath << QString::fromStdString(p);
    m_hints->setProject(QString::fromStdString(ax.dir), path, docpath, Settings::userLibraryDirs());
    if (m_langRef) m_langRef->refreshLibraries();
}

void QScriptMain::closeProject() {
    if (m_startupAx.isEmpty()) return;
    if (m_worker->isRunning() || m_paused) {
        setStatusMessage(QStringLiteral("运行中，无法关闭工程"));
        return;
    }
    logSys(QStringLiteral("关闭工程: ") + (m_projectName.isEmpty() ? m_startupAx : m_projectName));
    clearProject();
    updateModeUi();
}

void QScriptMain::closeFolder() {
    if (m_dir.isEmpty()) return;
    if (m_worker->isRunning() || m_paused) {
        setStatusMessage(QStringLiteral("运行中，无法关闭文件夹"));
        return;
    }
    logSys(QStringLiteral("关闭文件夹: ") + m_dir);
    clearProject();
    m_dir.clear();
    refreshHintContext();
    m_fileTree->clearRoot();
    ui->dockFiles->setVisible(false);
    m_mode = m_editorArea->count() > 0 ? WorkMode::File : WorkMode::None;
    updateModeUi();
}

void QScriptMain::updateModeUi() {
    if (m_closeProjectAction) m_closeProjectAction->setEnabled(!m_startupAx.isEmpty());
    if (m_closeFolderAction) m_closeFolderAction->setEnabled(!m_dir.isEmpty());
    updateWindowTitle();
    updateDockTitle();
    updateStatusBar();
}

void QScriptMain::updateWindowTitle() {
    if (m_mode == WorkMode::File && m_editorArea && m_editorArea->count() == 0)
        m_mode = WorkMode::None;
    QString title = QStringLiteral("AScript");
    switch (m_mode) {
    case WorkMode::Project:
        title += QStringLiteral(" — 工程: ") + m_projectName;
        break;
    case WorkMode::Folder:
        title += QStringLiteral(" — 目录: ") + QFileInfo(m_dir).fileName();
        break;
    case WorkMode::File: {
        const QString cur = m_editorArea ? m_editorArea->currentPath() : QString();
        title += QStringLiteral(" — ") +
                 (cur.isEmpty() ? tr("未命名") : QFileInfo(cur).fileName());
        break;
    }
    default: break;
    }
    setWindowTitle(title);
}

void QScriptMain::updateDockTitle() {
    QString title = tr("目录");
    QString tip = m_dir;
    if (m_mode == WorkMode::Project) {
        title = tr("工程: ") + m_projectName;
        tip = m_dir + QStringLiteral("\n.ax: ") + m_startupAx;
    } else if (!m_dir.isEmpty()) {
        title = tr("目录: ") + QFileInfo(m_dir).fileName();
    }
    ui->dockFiles->setWindowTitle(title);
    ui->dockFiles->setToolTip(tip);
}

QString QScriptMain::modeText() const {
    switch (m_mode) {
    case WorkMode::Project:
        return tr("工程: ") + m_projectName;
    case WorkMode::Folder:
        return tr("目录: ") + QFileInfo(m_dir).fileName();
    case WorkMode::File: {
        const QString cur = m_editorArea ? m_editorArea->currentPath() : QString();
        return tr("文件: ") + (cur.isEmpty() ? tr("未命名") : QFileInfo(cur).fileName());
    }
    default:
        return tr("就绪");
    }
}

void QScriptMain::updateStatusBar() {
    if (m_mode == WorkMode::File && m_editorArea && m_editorArea->count() == 0)
        m_mode = WorkMode::None;
    m_statusLabel->setText(modeText());
}

// setStatusMessage — transient status text (errors / notices)
void QScriptMain::setStatusMessage(const QString& _msg) {
    m_statusLabel->setText(_msg);
}

void QScriptMain::showFileProps(const QString& _path) {
    PropsPanel::show(_path, this);
}

void QScriptMain::buildMenus() {
    // every action with a shortcut registers in m_actionById (shortcut table
    // id → action) so applySettingsFromConfig can rebind them on save
    auto reg = [this](QAction* _a, const char* _id) {
        _a->setShortcut(Settings::shortcut(QLatin1String(_id)));
        m_actionById.insert(QLatin1String(_id), _a);
    };
    auto bind = [this, &reg](QAction* _a, const char* _id, auto&& _slot) {
        connect(_a, &QAction::triggered, this, _slot);
        reg(_a, _id);
    };

    QMenu* fileMenu = menuBar()->addMenu("文件(&F)");
    bind(fileMenu->addAction("新建(&N)"), "file.new", [this] { newDocument(); });
    QAction* openAct = fileMenu->addAction("打开文件(&O)…", this, &QScriptMain::openFileDialog);
    reg(openAct, "file.open");
    fileMenu->addAction("打开目录(&D)…", this, &QScriptMain::openDirDialog);
    fileMenu->addAction("打开项目(&P)…", this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, "打开项目", QString(), "*.ax");
        if (path.isEmpty()) return;
        Settings::addRecentPath(QFileInfo(path).absoluteFilePath());
        openProject(path);
    });
    bind(fileMenu->addAction("保存(&S)"), "file.save", [this] { m_editorArea->saveCurrent(); });
    fileMenu->addSeparator();
    m_closeProjectAction = fileMenu->addAction("关闭工程(&C)", this, &QScriptMain::closeProject);
    m_closeFolderAction = fileMenu->addAction("关闭文件夹(&F)", this, &QScriptMain::closeFolder);
    m_closeProjectAction->setEnabled(false);
    m_closeFolderAction->setEnabled(false);
    fileMenu->addSeparator();
    bind(fileMenu->addAction("退出(&X)"), "file.exit", [this] { close(); });

    QMenu* editMenu = menuBar()->addMenu("编辑(&E)");
    // edit actions target the current editor explicitly (menu + shortcut)
    auto editSlot = [this](auto _op) {
        return [this, _op] { if (ScriptEditor* ed = m_editorArea->currentEditor()) (ed->*_op)(); };
    };
    bind(editMenu->addAction("撤销(&U)"), "edit.undo", editSlot(&QPlainTextEdit::undo));
    bind(editMenu->addAction("重做(&R)"), "edit.redo", editSlot(&QPlainTextEdit::redo));
    editMenu->addSeparator();
    bind(editMenu->addAction("剪切(&T)"), "edit.cut", editSlot(&QPlainTextEdit::cut));
    bind(editMenu->addAction("复制(&C)"), "edit.copy", editSlot(&QPlainTextEdit::copy));
    bind(editMenu->addAction("粘贴(&P)"), "edit.paste", editSlot(&QPlainTextEdit::paste));

    QMenu* runMenu = menuBar()->addMenu("运行(&R)");
    m_runAction = runMenu->addAction("调试运行", this, [this] {
        if (m_paused) { // breakpoint hit: F5 resumes
            m_worker->continueRun();
            leavePauseUi();
        } else {
            runCurrent(true);
        } });
    reg(m_runAction, "debug.run");
    m_runNoDebugAction = runMenu->addAction("运行", this, [this] { runCurrent(false); });
    reg(m_runNoDebugAction, "debug.runNoDebug");
    m_rerunAction = runMenu->addAction("重新运行", this, [this] { runAgain(); });
    m_rerunAction->setEnabled(false);
    reg(m_rerunAction, "debug.rerun");
    runMenu->addSeparator();

    auto stepAction = [this, &reg, runMenu](const QString& _text, const char* _id, int _kind) {
        QAction* a = runMenu->addAction(_text, this, [this, _kind] { step(_kind); });
        a->setEnabled(false);
        reg(a, _id);
        m_stepActions.append(a);
        return a;
    };
    stepAction("步过", "debug.stepOver", 0);
    stepAction("步入", "debug.stepInto", 1);
    stepAction("步出", "debug.stepOut", 2);
    m_runToCursorAction = runMenu->addAction("运行到光标", this, [this] { runToCursor(); });
    m_runToCursorAction->setEnabled(false);
    reg(m_runToCursorAction, "debug.runToCursor");
    runMenu->addSeparator();

    m_consoleAction = runMenu->addAction("打开控制台", this, [this] {
        if (m_worker->isRunning()) {
            logSys(QStringLiteral("已有运行中的会话"));
            return;
        }
        if (!m_worker->openConsole()) {
            logSys(QStringLiteral("控制台启动失败"));
            return;
        }
        m_terminal->clear();
        ui->dockOutput->setVisible(true);
        showDebugBar();
        m_stopBtn->setEnabled(true);
        m_statusLabel->setText(QStringLiteral("控制台启动中…"));
    });
    reg(m_consoleAction, "console.open");
    m_consoleResetAction = runMenu->addAction("重置控制台", this, [this] {
        if (!m_worker->consoleActive()) {
            logSys(QStringLiteral("当前没有控制台会话"));
            return;
        }
        m_worker->resetConsole();
        logSys(QStringLiteral("控制台状态已重置"));
    });
    reg(m_consoleResetAction, "console.reset");
    runMenu->addSeparator();

    m_openLogAction = runMenu->addAction("打开会话日志", this, [this] {
        const QString path = m_worker->logPath();
        if (!path.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });
    m_openLogAction->setEnabled(false);
    runMenu->addAction("清空所有日志", this, [this] { clearAllLogs(); });
    runMenu->addSeparator();

    m_logpointAction = runMenu->addAction("日志断点（当前行）", this, [this] {
        ScriptEditor* ed = m_editorArea->currentEditor();
        const QString path = m_editorArea->currentPath();
        if (!ed || path.isEmpty()) {
            logSys(QStringLiteral("日志断点需要已保存的文件"));
            return;
        }
        const int row = ed->textCursor().blockNumber() + 1;
        bool ok = false;
        const QString text = QInputDialog::getText(this, QStringLiteral("日志断点"),
                                                   QStringLiteral("命中时打印（留空则删除该日志）："),
                                                   QLineEdit::Normal,
                                                   BreakpointStore::instance().log(path, row), &ok);
        if (!ok) return;
        BreakpointStore::instance().setLog(path, row, text);
        ed->setBreakpoints(BreakpointStore::instance().lines(path));
        m_worker->syncBreakpoints(path);
        logSys(QStringLiteral("%1日志断点 %2:%3")
                   .arg(text.isEmpty() ? QStringLiteral("删除") : QStringLiteral("设置"),
                        QFileInfo(path).fileName())
                   .arg(row));
    });
    reg(m_logpointAction, "debug.logpoint");
    m_conditionAction = runMenu->addAction("条件断点（当前行）", this, [this] {
        ScriptEditor* ed = m_editorArea->currentEditor();
        const QString path = m_editorArea->currentPath();
        if (!ed || path.isEmpty()) {
            logSys(QStringLiteral("条件断点需要已保存的文件"));
            return;
        }
        const int row = ed->textCursor().blockNumber() + 1;
        bool ok = false;
        const QString text = QInputDialog::getText(
            this, QStringLiteral("条件断点"),
            QStringLiteral("例如 i == 5（局部变量优先；留空删除）："), QLineEdit::Normal,
            BreakpointStore::instance().condition(path, row), &ok);
        if (!ok) return;
        BreakpointStore::instance().setCondition(path, row, text);
        ed->setBreakpoints(BreakpointStore::instance().lines(path));
        m_worker->syncBreakpoints(path);
        logSys(QStringLiteral("%1条件断点 %2:%3")
                   .arg(text.isEmpty() ? QStringLiteral("删除") : QStringLiteral("设置"),
                        QFileInfo(path).fileName())
                   .arg(row));
    });
    reg(m_conditionAction, "debug.condition");
    runMenu->addSeparator();

    m_interruptAction = runMenu->addAction("中断", this, [this] { interruptSession(); });
    reg(m_interruptAction, "debug.interrupt");
    m_interruptAction->setEnabled(false);

    QMenu* toolMenu = menuBar()->addMenu("工具(&T)");
    bind(toolMenu->addAction("查看 AST(&A)"), "view.ast", [this] { showAst(); });
    bind(toolMenu->addAction("编译(&B)…"), "tool.compile",
         [this] { openCompilePanel(QString(), false); });
    bind(toolMenu->addAction("格式化(&F)"), "tool.format", [this] { formatCurrent(); });
    QMenu* hintMenu = toolMenu->addMenu("提示缓存");
    hintMenu->addAction("重载", this, [this] {
        refreshHintContext();
        m_hints->reload(m_editorArea->hintDocuments());
        logSys(QStringLiteral("提示缓存已重载: %1 条").arg(m_hints->size()));
    });
    hintMenu->addAction("清空", this, [this] {
        m_hints->clear();
        if (m_langRef) m_langRef->refreshLibraries();
        logSys(QStringLiteral("提示缓存已清空"));
    });

    QMenu* viewMenu = menuBar()->addMenu("视图(&V)");
    viewMenu->addAction(ui->dockFiles->toggleViewAction());
    viewMenu->addAction(ui->dockOutput->toggleViewAction());
    viewMenu->addAction(ui->dockDebug->toggleViewAction());
    viewMenu->addAction(m_perfDock->toggleViewAction());
    viewMenu->addAction(m_langDock->toggleViewAction());

    QMenu* helpMenu = menuBar()->addMenu("帮助(&H)");
    helpMenu->addAction("语言参考(&L)", this, &QScriptMain::showLangRef);
    helpMenu->addAction("关于 AScript(&A)", this, &QScriptMain::about);
}

void QScriptMain::buildDebugBar() {
    m_debugBar = new EmbedWidget(m_editorArea);
    m_debugBar->setToolTip(QStringLiteral("拖动"));
    auto* lay = new QHBoxLayout(m_debugBar);
    lay->setContentsMargins(8, 4, 8, 4);
    lay->setSpacing(6);

    auto* grip = new QLabel(QStringLiteral("⋮⋮"), m_debugBar);
    grip->setStyleSheet(QStringLiteral("color:#928374;"));
    grip->setAttribute(Qt::WA_TransparentForMouseEvents); // press falls through to the bar
    lay->addWidget(grip);

    auto group = [this, lay] { // one recessed frame per button family
        auto* frame = new QFrame(m_debugBar);
        frame->setProperty("debugGroup", true);
        auto* row = new QHBoxLayout(frame);
        row->setContentsMargins(2, 2, 2, 2);
        row->setSpacing(2);
        lay->addWidget(frame);
        return row;
    };
    auto addButton = [](QHBoxLayout* _row, QToolButton* _btn) {
        _btn->setIconSize(QSize(16, 16));
        _btn->setFixedSize(28, 24);
        _row->addWidget(_btn);
    };

    { // run control: stop + pause/continue
        auto* row = group();
        m_stopBtn = new QToolButton(m_debugBar);
        m_stopBtn->setIcon(gui::icons::load(QStringLiteral("stop")));
        m_stopBtn->setToolTip(QStringLiteral("停止当前运行"));
        m_stopBtn->setEnabled(false);
        connect(m_stopBtn, &QToolButton::clicked, this, [this] { interruptSession(); });
        addButton(row, m_stopBtn);

        m_pauseBtn = new QToolButton(m_debugBar);
        m_pauseBtn->setCheckable(true);
        m_pauseBtn->setEnabled(false);
        applyPauseState(false);
        connect(m_pauseBtn, &QToolButton::clicked, this, [this](bool _on) {
            if (_on) {
                m_worker->pauseRun(); // flag set; blocks at next exec checkpoint
            } else {
                m_worker->continueRun(); // resume from pause
                leavePauseUi();
            }
        });
        addButton(row, m_pauseBtn);
    }

    { // stepping: over / into / out
        auto* row = group();
        static const char* kStepIcons[] = {"step-over", "step-into", "step-out"};
        for (int i = 0; i < m_stepActions.size(); ++i) {
            m_stepActions[i]->setIcon(gui::icons::load(QLatin1String(kStepIcons[i])));
            auto* btn = new QToolButton(m_debugBar); // enabled state stays with the action
            btn->setDefaultAction(m_stepActions[i]);
            btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
            addButton(row, btn);
        }
    }

    { // navigation: run to cursor
        auto* row = group();
        m_runToCursorAction->setIcon(gui::icons::load(QStringLiteral("run-to-cursor")));
        auto* btn = new QToolButton(m_debugBar);
        btn->setDefaultAction(m_runToCursorAction);
        btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
        addButton(row, btn);
    }

    { // toggle: skip traps
        auto* row = group();
        m_skipTrapBtn = new QToolButton(m_debugBar);
        m_skipTrapBtn->setIcon(gui::icons::load(QStringLiteral("skip-trap")));
        m_skipTrapBtn->setToolTip(QStringLiteral("跳过脚本中的 Trap（点击切换）"));
        m_skipTrapBtn->setCheckable(true);
        m_skipTrapBtn->setEnabled(false);
        connect(m_skipTrapBtn, &QToolButton::clicked, this,
                [this](bool _on) { m_worker->setSkipTraps(_on); });
        addButton(row, m_skipTrapBtn);
    }

    m_debugBar->hide();
    m_editorArea->installEventFilter(this);
}

void QScriptMain::showDebugBar() {
    m_debugBar->adjustSize(); // QSS padding applies after construction — refit every show
    m_debugBarTop = debug_bar_top(m_editorArea) + 4;
    m_debugBar->move_top_center(m_debugBarTop); // default: top-center, below the tab bar
    m_debugBar->raise();
    m_debugBar->show();
}

void QScriptMain::buildStatusBar() {
    m_statusLabel = new QLabel("就绪", this);
    ui->statusbar->addWidget(m_statusLabel);
    m_posLabel = new QLabel("行 1, 列 1", this);
    ui->statusbar->addPermanentWidget(m_posLabel);
}

void QScriptMain::buildHomeBar() {
    // leftmost vertical activity bar (VSCode-style): one toggle per dock,
    // packed into the .ui frame. Docks use plain setVisible
    // (toggleViewAction proved unreliable here).
    auto* lay = new QVBoxLayout(ui->frame);
    lay->setContentsMargins(3, 3, 3, 3);
    lay->setSpacing(3);

    struct DockItem {
        QDockWidget* dock;
        const char* name;
        const char* icon;
    };
    const DockItem items[] = {
        {ui->dockFiles, "文件", "folder"},
        {ui->dockDebug, "调试", "bug"},
        {ui->dockOutput, "输出", "terminal"},
        {m_perfDock, "性能", "chart"},
        {m_langDock, "语言参考", "book"},
    };
    // checked = dock visible; active = the dock that currently owns the focus
    auto railButtons = std::make_shared<QHash<QDockWidget*, QToolButton*>>();
    auto setActive = [railButtons](QDockWidget* _dock) {
        for (auto it = railButtons->cbegin(); it != railButtons->cend(); ++it) {
            QToolButton* b = it.value();
            const bool on = it.key() == _dock;
            if (b->property("active").toBool() == on) continue;
            b->setProperty("active", on);
            b->style()->unpolish(b);
            b->style()->polish(b);
        }
    };
    for (const auto& item : items) {
        auto* btn = new QToolButton(ui->frame);
        btn->setIcon(gui::icons::load(item.icon));
        btn->setIconSize(QSize(28, 28));
        btn->setCheckable(true);
        btn->setChecked(!item.dock->isHidden()); // startup visibility (pre-show isVisible is false)
        btn->setProperty("rail", true);
        btn->setProperty("active", false);
        btn->setToolTip(item.name);
        btn->setAutoRaise(true);
        btn->setCursor(Qt::PointingHandCursor);
        (*railButtons)[item.dock] = btn;
        // toggle: dock visible <-> button checked, both directions
        connect(btn, &QToolButton::clicked, item.dock, [this, item, btn, setActive](bool _on) {
            if (item.dock == m_langDock && _on) showLangRef();
            else item.dock->setVisible(_on);
            if (_on) setActive(item.dock);
            else if (btn->property("active").toBool()) setActive(nullptr);
        });
        connect(item.dock, &QDockWidget::visibilityChanged, btn, [btn, setActive](bool _v) {
            btn->setChecked(_v);
            if (!_v && btn->property("active").toBool()) setActive(nullptr);
        });
        lay->addWidget(btn);
        // file tree toggle also controls .axp docks
        if (item.dock == ui->dockFiles) {
            connect(btn, &QToolButton::clicked, this, [this](bool _on) {
                for (auto* d : m_axpDocks) d->setVisible(_on);
            });
            connect(ui->dockFiles, &QDockWidget::visibilityChanged, this, [this](bool _v) {
                for (auto* d : m_axpDocks) d->setVisible(_v);
            });
        }
    }
    connect(qApp, &QApplication::focusChanged, this, [railButtons, setActive](QWidget*, QWidget* _now) {
        QDockWidget* owner = nullptr;
        if (_now)
            for (auto it = railButtons->cbegin(); it != railButtons->cend(); ++it)
                if (_now == it.key() || it.key()->isAncestorOf(_now)) {
                    owner = it.key();
                    break;
                }
        setActive(owner);
    });

    lay->addStretch(); // pushes the settings button to the bottom

    auto* settingsBtn = new QToolButton(ui->frame);
    settingsBtn->setIcon(gui::icons::load("gear"));
    settingsBtn->setIconSize(QSize(28, 28));
    settingsBtn->setProperty("rail", true);
    settingsBtn->setToolTip("设置");
    settingsBtn->setAutoRaise(true);
    settingsBtn->setCursor(Qt::PointingHandCursor);
    connect(settingsBtn, &QToolButton::clicked, this, [this] {
        m_setting->move_center();
        m_setting->show(); // modal: blocks this window, see QScriptSetting::showEvent
    });
    lay->addWidget(settingsBtn);
}

void QScriptMain::about() {
    QMessageBox::about(this, "关于 AScript",
                       "AScript — 基于 Alexis 引擎的脚本编辑器 / 调试器");
}

void QScriptMain::showLangRef() {
    m_langDock->setVisible(true);
    m_langDock->raise();
}

void QScriptMain::showAst() {
    ScriptEditor* ed = m_editorArea->currentEditor();
    if (!ed) return;
    if (ed->isAstMode()) { // "返回原文"
        ed->restoreSource();
        return;
    }
    const std::string src = ed->toPlainText().toStdString();
    alx::bytes b(src);
    const std::string ast = alx::script::engine::prtast(alx::bytes_view(b));
    if (ast.rfind("Error:", 0) == 0) {
        m_terminal->append(QString::fromStdString(ast)); // parse error → output dock
        ui->dockOutput->setVisible(true);
        return;
    }
    ed->showAst(QString::fromStdString(ast)); // read-only AST view in the current doc
}

void QScriptMain::formatCurrent() {
    ScriptEditor* ed = m_editorArea->currentEditor();
    if (!ed || ed->isAstMode()) return;
    const std::string src = ed->toPlainText().toStdString();
    alx::bytes b(src);
    const std::string fmt = alx::script::engine::prtfmt(alx::bytes_view(b)).to_string();
    if (fmt.empty() || fmt == src) return;
    QTextCursor c = ed->textCursor();
    c.select(QTextCursor::Document);
    c.insertText(QString::fromStdString(fmt)); // undoable, marks dirty via textChanged
    ed->setTextCursor(c);
}

// runAx — .ax config-driven run: parse errors → status bar; engine runs on
// the worker thread (same path as runText, trigger differs)
void QScriptMain::runAx(const QString& _axPath, bool _debug) {
    if (m_worker->isRunning()) return;
    m_lastDebug = _debug;
    m_hasLastRun = true;
    if (m_rerunAction) m_rerunAction->setEnabled(false);
    m_runningAxp.clear(); // .ax run — no .axp breakpoint binding
    logSys("运行项目: " + _axPath);
    m_terminal->clear();
    ui->dockOutput->setVisible(true);
    ui->dockDebug->setVisible(true);
    showDebugBar();
    m_statusLabel->setText("运行中… " + QFileInfo(_axPath).fileName());
    m_runAction->setEnabled(false);
    m_stopBtn->setEnabled(true);
    m_pauseBtn->setEnabled(true);
    applyPauseState(false);
    m_skipTrapBtn->setEnabled(true);
    resetSkipTrap();
    setStepActionsEnabled(false);
    m_runNoDebugAction->setEnabled(false);
    m_runToCursorAction->setEnabled(false);
    QMetaObject::invokeMethod(m_worker, "runAx", Qt::QueuedConnection, Q_ARG(QString, _axPath),
                              Q_ARG(bool, _debug));
}

void QScriptMain::runCurrent(bool _debug) {
    if (m_paused || m_worker->isRunning()) return;
    m_lastDebug = _debug;
    m_hasLastRun = true;
    if (m_rerunAction) m_rerunAction->setEnabled(false);
    if (!m_startupAx.isEmpty()) { // startup project wins
        runAx(m_startupAx, _debug);
        return;
    }
    ScriptEditor* ed = m_editorArea->currentEditor();
    if (!ed) return;
    const QString path = m_editorArea->currentPath();
    logSys("运行: " + (path.isEmpty() ? "未命名文档" : path));
    m_terminal->clear();
    // running: surface the output + debug docks (visibilityChanged syncs the buttons)
    ui->dockOutput->setVisible(true);
    ui->dockDebug->setVisible(true);
    showDebugBar();
    m_runningAxp = QFileInfo(path).suffix() == "axp" ? path : QString(); // .axp breakpoint binding
    ed->clearExecutionLine();
    m_statusLabel->setText("运行中…");
    m_runAction->setEnabled(false);
    m_stopBtn->setEnabled(true);
    m_pauseBtn->setEnabled(true);
    applyPauseState(false);
    m_skipTrapBtn->setEnabled(true);
    resetSkipTrap();
    // dirty doc (or untitled) runs the editor text; clean saved doc runs the file
    // QueuedConnection: a direct call would run the whole engine (incl. trap
    // blocking) on the UI thread and freeze the window at a breakpoint
    const QString code = (path.isEmpty() || m_editorArea->currentDirty()) ? ed->toPlainText() : QString();
    const QString file = path; // a dirty buffer keeps its file's identity for breakpoints/imports
    setStepActionsEnabled(false);
    m_runNoDebugAction->setEnabled(false);
    m_runToCursorAction->setEnabled(false);
    QMetaObject::invokeMethod(m_worker, "runText", Qt::QueuedConnection,
                              Q_ARG(QString, code), Q_ARG(QString, file), Q_ARG(bool, _debug));
}

// runAgain — restart the current script with the last run's debug flag
void QScriptMain::runAgain() {
    if (m_worker->isRunning() || !m_hasLastRun) return;
    if (!m_startupAx.isEmpty())
        runAx(m_startupAx, m_lastDebug);
    else
        runCurrent(m_lastDebug);
}

// runFile — the per-file run exception: run the clicked file, not the project
void QScriptMain::runFile(const QString& _path, bool _debug) {
    if (QFileInfo(_path).suffix() == "ax") {
        runAx(_path, _debug);
        return;
    }
    if (m_paused || m_worker->isRunning()) return;
    m_lastDebug = _debug;
    m_hasLastRun = true;
    if (m_rerunAction) m_rerunAction->setEnabled(false);
    const bool isAxp = QFileInfo(_path).suffix() == "axp";
    if (isAxp)
        lockAxpView(_path, "@::", QString()); // open dock + [::] dump
    else
        openFile(_path);
    // dirty buffer wins, matching runCurrent's file-run contract
    QString code;
    if (!isAxp && m_editorArea->currentDirty() &&
        QFileInfo(m_editorArea->currentPath()).absoluteFilePath() ==
            QFileInfo(_path).absoluteFilePath()) {
        if (ScriptEditor* ed = m_editorArea->currentEditor()) code = ed->toPlainText();
    }
    logSys((_debug ? QStringLiteral("调试运行: ") : QStringLiteral("运行: ")) + _path);
    m_terminal->clear();
    ui->dockOutput->setVisible(true);
    ui->dockDebug->setVisible(true);
    showDebugBar();
    m_runningAxp = isAxp ? QFileInfo(_path).absoluteFilePath() : QString();
    m_statusLabel->setText("运行中…");
    m_runAction->setEnabled(false);
    m_stopBtn->setEnabled(true);
    m_pauseBtn->setEnabled(true);
    applyPauseState(false);
    m_skipTrapBtn->setEnabled(true);
    resetSkipTrap();
    setStepActionsEnabled(false);
    m_runNoDebugAction->setEnabled(false);
    m_runToCursorAction->setEnabled(false);
    QMetaObject::invokeMethod(m_worker, "runText", Qt::QueuedConnection, Q_ARG(QString, code),
                              Q_ARG(QString, _path), Q_ARG(bool, _debug));
}

// leavePauseUi — pause exit shared by resume, step and run-to-cursor
void QScriptMain::leavePauseUi() {
    m_paused = false;
    applyPauseState(false);
    m_runAction->setText("调试运行(&F5)");
    m_interruptAction->setEnabled(false);
    m_editorArea->clearExecutionLines();
    setStepActionsEnabled(false);
    m_runToCursorAction->setEnabled(false);
    m_statusLabel->setText("运行中…");
}

// step — leave the pause UI and ask the session for one step
void QScriptMain::step(int _kind) {
    if (!m_paused || m_worker->focusThread() != 0) return;
    leavePauseUi();
    if (_kind == 0) m_worker->stepOver();
    else if (_kind == 1) m_worker->stepInto();
    else m_worker->stepOut();
}

// runToCursor — temporary breakpoint at the cursor, then continue
void QScriptMain::runToCursor() {
    if (!m_paused || m_worker->focusThread() != 0) return;
    ScriptEditor* ed = m_editorArea->currentEditor();
    const QString path = m_editorArea->currentPath();
    if (!ed || path.isEmpty()) {
        logSys(QStringLiteral("运行到光标需要已保存的文件"));
        return;
    }
    const int row = ed->textCursor().blockNumber() + 1;
    leavePauseUi();
    m_statusLabel->setText(QString("运行到第 %1 行…").arg(row));
    m_worker->runToCursor(path, row);
}

// interruptSession — stop the live run; menu and debug bar share this path
void QScriptMain::interruptSession() {
    m_worker->interruptRun();
    m_paused = false;
    m_interruptAction->setEnabled(false);
    m_statusLabel->setText("中断中…");
}

// fillModuleList — root modules from the paused run's module snapshot
void QScriptMain::fillModuleList(const QVariantList& _nodes) {
    m_debugTree->clear();
    for (const auto& nv : _nodes) {
        const QVariantMap n = nv.toMap();
        auto* item = new QTreeWidgetItem(m_debugTree);
        item->setText(0, n.value(QStringLiteral("name")).toString());
        // only modules have a path and a variable view; a link's value is a placeholder
        const bool is_module = n.value(QStringLiteral("kind")).toString() == QLatin1String("module");
        const QString path = is_module ? n.value(QStringLiteral("value")).toString() : QString();
        item->setData(0, Qt::UserRole, path);
        item->setData(0, Qt::UserRole + 1, n.value(QStringLiteral("ref")).toString());
        if (is_module) {
            if (!path.isEmpty()) item->setToolTip(0, path);
        } else {
            item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
            item->setForeground(0, m_debugTree->palette().color(QPalette::Disabled, QPalette::Text));
            item->setToolTip(0, QStringLiteral("link 无变量视图"));
        }
    }
    if (m_debugTree->topLevelItemCount() == 0) { // placeholder while the snapshot is empty
        auto* empty = new QTreeWidgetItem(m_debugTree);
        empty->setText(0, QStringLiteral("无模块数据"));
        empty->setFlags(Qt::NoItemFlags);
        empty->setForeground(0, m_debugTree->palette().color(QPalette::Disabled, QPalette::Text));
    }
    m_debugTree->expandAll();
    // the modules snapshot lags the stack; replay the frame's module highlight
    if (m_scope == scope::frame)
        if (QTreeWidgetItem* item = m_debugStack->currentItem())
            highlightModule(item->data(0, Qt::UserRole + 1).toString(),
                            item->data(0, Qt::UserRole + 2).toString());
}

// fillThreadList — one stop wave's captured threads, main as the root
void QScriptMain::fillThreadList(const QVariantList& _threads, quint64 _stop, int _known,
                                 int _captured) {
    Q_UNUSED(_stop);
    const quint64 focus = m_worker->focusThread();
    m_threadTree->clear();
    QHash<qulonglong, QTreeWidgetItem*> nodes;
    QList<QVariantMap> items;
    for (const auto& tv : _threads) {
        const QVariantMap t = tv.toMap();
        const quint64 id = t.value(QStringLiteral("id")).toULongLong();
        auto* item = new QTreeWidgetItem;
        const QString label = t.value(QStringLiteral("label")).toString();
        item->setText(0, !label.isEmpty() ? label
                                          : (id == 0 ? QStringLiteral("主线程")
                                                     : QStringLiteral("线程 %1").arg(id)));
        const qulonglong row = t.value(QStringLiteral("row")).toULongLong();
        const qulonglong col = t.value(QStringLiteral("col")).toULongLong();
        const qulonglong ofst = t.value(QStringLiteral("ofst")).toULongLong();
        QString pos;
        if (row) {
            pos = QStringLiteral("%1:%2").arg(row).arg(col);
            if (ofst) pos += QStringLiteral(" (+%1)").arg(ofst);
        }
        item->setText(1, pos);
        item->setData(0, Qt::UserRole, static_cast<qulonglong>(id));
        if (t.value(QStringLiteral("trigger")).toBool()) {
            QFont bold = item->font(0);
            bold.setBold(true);
            item->setFont(0, bold);
            item->setFont(1, bold);
        }
        const QString args = t.value(QStringLiteral("trapArgs")).toString();
        if (!args.isEmpty()) item->setToolTip(0, args);
        nodes.insert(id, item);
        items.append(t);
    }
    for (const QVariantMap& t : items) {
        const quint64 id = t.value(QStringLiteral("id")).toULongLong();
        const qint64 parentId = t.value(QStringLiteral("parent")).toLongLong();
        QTreeWidgetItem* item = nodes.value(id);
        QTreeWidgetItem* parent =
            parentId >= 0 ? nodes.value(static_cast<qulonglong>(parentId)) : nullptr;
        if (parent && parent != item)
            parent->addChild(item);
        else
            m_threadTree->addTopLevelItem(item);
    }
    m_threadTree->expandAll();
    if (QTreeWidgetItem* current = nodes.value(focus)) m_threadTree->setCurrentItem(current);
    m_threadLabel->setText(QStringLiteral("线程 %1/%2 已捕获")
                               .arg(_captured)
                               .arg(_known > 0 ? _known : 1));
}

// fillFrameList — call stack, innermost frame first
void QScriptMain::fillFrameList(const QVariantList& _frames) {
    m_debugStack->clear();
    for (const auto& fv : _frames) {
        const QVariantMap f = fv.toMap();
        auto* item = new QTreeWidgetItem(m_debugStack);
        const QString func = f.value(QStringLiteral("func")).toString();
        const bool internal = f.value(QStringLiteral("internal")).toBool();
        const bool topLevel = !internal && (func.isEmpty() || func == QLatin1String("?"));
        item->setText(0, internal ? QStringLiteral("[内部代码] %1").arg(func.isEmpty() ? QStringLiteral("?") : func)
                                  : (topLevel ? QStringLiteral("顶层") : func));
        item->setData(0, Qt::UserRole + 4, topLevel);
        if (internal) {
            item->setForeground(0, m_debugStack->palette().color(QPalette::Disabled, QPalette::Text));
            item->setToolTip(0, QStringLiteral("内部代码帧，无法查看变量"));
        }
        const QString file = f.value(QStringLiteral("file")).toString();
        const int line = f.value(QStringLiteral("line")).toInt();
        QString pos = file.isEmpty() ? QString() : QFileInfo(file).fileName();
        if (line > 0) pos += QStringLiteral(":%1").arg(line);
        item->setText(1, pos);
        item->setData(0, Qt::UserRole, f.value(QStringLiteral("ref")));
        item->setData(0, Qt::UserRole + 1, file);
        item->setData(0, Qt::UserRole + 2, f.value(QStringLiteral("alias")));
        item->setData(0, Qt::UserRole + 3, line);
        if (!file.isEmpty())
            item->setToolTip(1, line > 0 ? QStringLiteral("%1:%2").arg(file).arg(line) : file);
    }
}

// selectFrame — variables follow the clicked frame
void QScriptMain::selectFrame(const QString& _ref) {
    if (_ref.isEmpty()) return;
    m_scope = scope::frame;
    m_varsModel->setRoot(_ref);
    m_debugTree->clearSelection();
    QString name = _ref;
    bool topLevel = false;
    for (int i = 0; i < m_debugStack->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_debugStack->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() == _ref) {
            name = item->text(0);
            topLevel = item->data(0, Qt::UserRole + 4).toBool();
            highlightModule(item->data(0, Qt::UserRole + 1).toString(),
                            item->data(0, Qt::UserRole + 2).toString());
            break;
        }
    }
    m_scopeLabel->setText(frameScopeText(name, topLevel));
}

// selectModule — variables follow the clicked module
void QScriptMain::selectModule(const QString& _ref) {
    if (_ref.isEmpty()) return;
    m_scope = scope::module;
    m_varsModel->setRoot(_ref);
    m_debugStack->clearSelection();
    highlightModule(QString(), QString());
    QTreeWidgetItem* item = m_debugTree->currentItem();
    m_scopeLabel->setText(QStringLiteral("作用域: 模块 ") + (item ? item->text(0) : _ref));
}

// highlightModule — mark the module owning the frame: path first, alias second
void QScriptMain::highlightModule(const QString& _source, const QString& _alias) {
    const QString path = _source.startsWith(QLatin1String("file:")) ? _source.mid(5) : QString();
    const QString clean = path.isEmpty() ? QString() : QDir::cleanPath(path);
    const auto modulePath = [this](QTreeWidgetItem* _item) {
        return _item->data(0, Qt::UserRole).toString();
    };
    QTreeWidgetItem* hit = nullptr;
    for (int i = 0; i < m_debugTree->topLevelItemCount() && !hit; ++i) {
        QTreeWidgetItem* item = m_debugTree->topLevelItem(i);
        const QString itemPath = modulePath(item);
        if (!clean.isEmpty() && !itemPath.isEmpty() && QDir::cleanPath(itemPath) == clean) hit = item;
    }
    for (int i = 0; i < m_debugTree->topLevelItemCount() && !hit; ++i) {
        QTreeWidgetItem* item = m_debugTree->topLevelItem(i);
        if (!_alias.isEmpty() && !modulePath(item).isEmpty() && item->text(0) == _alias) hit = item;
    }
    const QColor mark = m_debugTree->palette().color(QPalette::Highlight);
    for (int i = 0; i < m_debugTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_debugTree->topLevelItem(i);
        const bool on = item == hit;
        QFont font = item->font(0);
        font.setBold(on);
        item->setFont(0, font);
        item->setBackground(0, on ? QBrush(QColor(mark.red(), mark.green(), mark.blue(), 70)) : QBrush());
    }
    if (hit) m_debugTree->scrollToItem(hit);
}

void QScriptMain::setStepActionsEnabled(bool _on) {
    for (QAction* a : m_stepActions) a->setEnabled(_on);
}

void QScriptMain::applyPauseState(bool _paused) {
    m_pauseBtn->setChecked(_paused);
    m_pauseBtn->setIcon(gui::icons::load(_paused ? QStringLiteral("play") : QStringLiteral("pause")));
    m_pauseBtn->setToolTip(_paused ? QStringLiteral("继续") : QStringLiteral("暂停"));
}

void QScriptMain::resetSkipTrap() {
    m_skipTrapBtn->setChecked(false);
    if (m_worker) m_worker->setSkipTraps(false);
}

// openCompilePanel — ax/axc compile dialog (scope/target preselected by caller)
void QScriptMain::openCompilePanel(const QString& _selectedFile, bool _treeFirst) {
    auto* panel = new CompilePanel(
        this, m_dir, m_startupAx, m_projectName, m_editorArea->currentPath(),
        [this](const QString& _path) { return m_editorArea->isDirty(_path); }, _selectedFile,
        _treeFirst);
    connect(panel, &CompilePanel::logMessage, this, &QScriptMain::logSys);
    connect(panel, &EmbedWidget::sig_visible, panel,
            [panel](bool _v) { if (!_v) panel->deleteLater(); });
    panel->adjustSize();
    panel->move_center();
    panel->show();
}
