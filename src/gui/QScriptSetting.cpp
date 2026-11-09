#include "QScriptSetting.h"
#include "ui_QScriptSetting.h"

#include "proto/config.h"
#include "Settings.h"
#include "QtCompat.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFontDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeySequenceEdit>
#include <QListWidget>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QShowEvent>
#include <QSpinBox>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
    const char* kNavNames[] = {"编辑器", "输出", "快捷键", "代码提示", "运行", "库"};

    // MaskWidget — dim overlay blocking the parent while the panel is open
    // (lock-window style, RegularParse pattern): the parent stays enabled, so
    // its children (including this panel) keep working
    class MaskWidget : public QWidget {
    public:
        explicit MaskWidget(QWidget* _parent) : QWidget(_parent) {
            setStyleSheet(QStringLiteral("background: rgba(15, 15, 15, 150);"));
        }

    protected:
        void resizeEvent(QResizeEvent*) override {
            setGeometry(parentWidget()->rect()); // track the parent
        }
    };
} // namespace

QScriptSetting::QScriptSetting(QWidget* parent)
    : EmbedWidget(parent), ui(new Ui::QScriptSetting) {
    ui->setupUi(this);

    ui->rootLayout->insertLayout(0, gui::titleRow(tr("设置"), this, [this] { hide(); }));
    ui->btnSave->setProperty("primary", true);

    buildPages();
    loadValues();
    connect(ui->btnSave, &QPushButton::clicked, this, [this] {
        saveValues();
        hide();
    });
    connect(ui->btnCancel, &QPushButton::clicked, this, [this] { hide(); });
    connect(ui->btnReset, &QPushButton::clicked, this, [this] { resetAll(); });
    hide();
}

QScriptSetting::~QScriptSetting() {
    delete ui;
}

void QScriptSetting::showEvent(QShowEvent* _ev) {
    loadValues(); // re-read on every open — external changes win
    if (parentWidget()) {
        if (!m_mask) {
            m_mask = new MaskWidget(parentWidget());
            m_mask->setGeometry(parentWidget()->rect());
        }
        m_mask->show();
        m_mask->raise();
        raise(); // panel above the mask
    }
    EmbedWidget::showEvent(_ev);
}

void QScriptSetting::hideEvent(QHideEvent* _ev) {
    if (m_mask) m_mask->hide();
    EmbedWidget::hideEvent(_ev);
}

void QScriptSetting::buildPages() {
    // left nav: one button per page, switching the stacked widget; a button
    // group keeps exactly one checked at a time
    auto* navLay = new QVBoxLayout(ui->scrollAreaWidgetContents);
    navLay->setContentsMargins(2, 4, 2, 4);
    navLay->setSpacing(4);
    auto* navGroup = new QButtonGroup(this); // exclusive by default
    for (int i = 0; i < 6; ++i) {
        auto* btn = new QToolButton(ui->scrollAreaWidgetContents);
        btn->setText(QString::fromUtf8(kNavNames[i]));
        btn->setProperty("nav", true);
        btn->setCheckable(true);
        btn->setAutoRaise(true);
        btn->setFixedHeight(24); // QSS has no min-height — explicit size keeps buttons uniform
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        if (i == 0) btn->setChecked(true);
        navGroup->addButton(btn);
        connect(btn, &QToolButton::clicked, this, [this, i] { ui->stackedWidget->setCurrentIndex(i); });
        navLay->addWidget(btn);
        m_navButtons[i] = btn;
    }
    navLay->addStretch();

    // ── page 0: editor font / size ──
    auto* editorForm = new QFormLayout(ui->page);
    editorForm->setContentsMargins(12, 12, 12, 12);
    m_fontBox = new QComboBox(ui->page);
    m_fontBox->setFixedHeight(24); // uniform input height (QSS lacks min-height)
    m_fontBox->addItem("Fira Code"); // bundled font first — the default option
    for (const QString& f : QFontDatabase().families())
        if (f != "Fira Code" && QFontInfo(QFont(f)).fixedPitch()) m_fontBox->addItem(f);
    auto* browseBtn = new QPushButton("浏览…", ui->page);
    browseBtn->setFixedSize(64, 24); // explicit size (QSS lacks min-height)
    connect(browseBtn, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QFont picked = QFontDialog::getFont(&ok, QFont(m_fontBox->currentText(), m_fontSize->value()), this);
        if (!ok) return;
        m_fontBox->setCurrentText(picked.family()); // picker may offer non-monospace fonts
    });
    auto* fontWidget = new QWidget(ui->page);
    auto* fontRow = new QHBoxLayout(fontWidget);
    fontRow->setContentsMargins(0, 0, 0, 0);
    fontRow->addWidget(m_fontBox, 1);
    fontRow->addWidget(browseBtn);
    editorForm->addRow("字体", fontWidget);
    m_fontSize = new QSpinBox(ui->page);
    m_fontSize->setFixedHeight(24); // uniform input height
    m_fontSize->setRange(8, 30);
    editorForm->addRow("字号", m_fontSize);
    m_uiScale = new QComboBox(ui->page);
    m_uiScale->setFixedHeight(24);
    for (int pct : {100, 125, 150, 175, 200})
        m_uiScale->addItem(QString::number(pct) + "%", pct);
    m_uiScale->setToolTip("重启后生效");
    editorForm->addRow("界面缩放(重启生效)", m_uiScale);
    editorForm->addItem(new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding));

    // ── page 1: output font size ──
    auto* outputForm = new QFormLayout(ui->page_2);
    outputForm->setContentsMargins(12, 12, 12, 12);
    m_outputSize = new QSpinBox(ui->page_2);
    m_outputSize->setFixedHeight(24); // uniform input height
    m_outputSize->setRange(8, 30);
    outputForm->addRow("输出字号(终端/日志/调试)", m_outputSize);
    outputForm->addItem(new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding));

    // ── page 2: shortcut table with name search ──
    auto* scPage = new QWidget(ui->stackedWidget);
    auto* scLay = new QVBoxLayout(scPage);
    scLay->setContentsMargins(8, 8, 8, 8);
    m_searchEdit = new QLineEdit(scPage);
    m_searchEdit->setFixedHeight(24); // uniform input height
    m_searchEdit->setPlaceholderText("搜索功能名称…");
    scLay->addWidget(m_searchEdit);
    m_shortcutTable = new QTableWidget(scPage);
    m_shortcutTable->setColumnCount(2);
    m_shortcutTable->setHorizontalHeaderLabels({"功能", "快捷键"});
    m_shortcutTable->horizontalHeader()->setStretchLastSection(true);
    m_shortcutTable->verticalHeader()->setVisible(false);
    m_shortcutTable->verticalHeader()->setDefaultSectionSize(24); // fits the key editors
    m_shortcutTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    scLay->addWidget(m_shortcutTable, 1);
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this](const QString& _t) {
        populateShortcutTable(_t.trimmed());
    });
    ui->stackedWidget->addWidget(scPage);

    // ── page 3: completion params ──
    auto* compPage = new QWidget(ui->stackedWidget);
    auto* compForm = new QFormLayout(compPage);
    compForm->setContentsMargins(12, 12, 12, 12);
    m_cbEnabled = new QCheckBox(compPage);
    compForm->addRow("启用代码提示", m_cbEnabled);
    m_cbDelay = new QSpinBox(compPage);
    m_cbDelay->setFixedHeight(24); // uniform input height
    m_cbDelay->setRange(50, 2000);
    m_cbDelay->setSuffix(" ms");
    compForm->addRow("弹出延迟", m_cbDelay);
    m_cbWindowItems = new QSpinBox(compPage);
    m_cbWindowItems->setFixedHeight(24); // uniform input height
    m_cbWindowItems->setRange(4, 30); // at least 4 rows
    compForm->addRow("窗口候选数(行)", m_cbWindowItems);
    m_cbListLimit = new QSpinBox(compPage);
    m_cbListLimit->setFixedHeight(24); // uniform input height
    m_cbListLimit->setRange(0, 1000); // 0 = unlimited
    compForm->addRow("列表上限(0=无上限)", m_cbListLimit);
    m_cbCacheLimit = new QSpinBox(compPage);
    m_cbCacheLimit->setFixedHeight(24); // uniform input height
    m_cbCacheLimit->setRange(0, 4096); // 0 = unlimited
    compForm->addRow("提示缓存条数(0=无上限)", m_cbCacheLimit);
    m_cbShowDesc = new QCheckBox(compPage);
    compForm->addRow("显示描述", m_cbShowDesc);
    m_completionSize = new QSpinBox(compPage);
    m_completionSize->setFixedHeight(24); // uniform input height
    m_completionSize->setRange(8, 30);
    compForm->addRow("提示框字号", m_completionSize);
    m_cbHover = new QCheckBox(compPage);
    compForm->addRow("选中提示", m_cbHover);
    m_hoverSize = new QSpinBox(compPage);
    m_hoverSize->setFixedHeight(24); // uniform input height
    m_hoverSize->setRange(8, 30);
    compForm->addRow("悬浮信息字号", m_hoverSize);
    compForm->addItem(new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding));
    ui->stackedWidget->addWidget(compPage);

    // ── page 4: runner / limits ──
    auto* runPage = new QWidget(ui->stackedWidget);
    auto* runForm = new QFormLayout(runPage);
    runForm->setContentsMargins(12, 12, 12, 12);
    m_runnerEnabled = new QCheckBox(runPage);
    runForm->addRow("启用脚本执行", m_runnerEnabled);
    m_runnerPath = new QLineEdit(runPage);
    m_runnerPath->setFixedHeight(24);
    m_runnerPath->setPlaceholderText("默认 <程序目录>/axc-runner");
    auto* runnerRowWidget = new QWidget(runPage);
    auto* runnerRow = new QHBoxLayout(runnerRowWidget);
    runnerRow->setContentsMargins(0, 0, 0, 0);
    runnerRow->addWidget(m_runnerPath, 1);
    auto* runnerBrowse = new QPushButton("浏览…", runPage);
    runnerBrowse->setFixedSize(64, 24);
    connect(runnerBrowse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, "选择 axc-runner", m_runnerPath->text());
        if (!path.isEmpty()) m_runnerPath->setText(path);
    });
    runnerRow->addWidget(runnerBrowse);
    runForm->addRow("runner 路径", runnerRowWidget);
    m_runWall = new QSpinBox(runPage);
    m_runWall->setFixedHeight(24);
    m_runWall->setRange(0, 3600000);
    m_runWall->setSingleStep(500);
    m_runWall->setSuffix(" ms");
    m_runWall->setSpecialValueText("不限");
    runForm->addRow("单次运行时限", m_runWall);
    m_runMem = new QSpinBox(runPage);
    m_runMem->setFixedHeight(24);
    m_runMem->setRange(0, 65536);
    m_runMem->setSuffix(" MB");
    m_runMem->setSpecialValueText("不限");
    runForm->addRow("runner 内存上限", m_runMem);
    m_debugOn = new QCheckBox(runPage);
    runForm->addRow("默认调试模式", m_debugOn);
    m_mtDebug = new QCheckBox(runPage);
    m_mtDebug->setToolTip("仅调试运行生效");
    runForm->addRow("多线程调试", m_mtDebug);
    m_logKeep = new QSpinBox(runPage);
    m_logKeep->setFixedHeight(24);
    m_logKeep->setRange(0, 365);
    m_logKeep->setSuffix(" 天");
    m_logKeep->setSpecialValueText("不清理");
    runForm->addRow("会话日志保留", m_logKeep);

    auto toggle_row = [runPage](QCheckBox*& _on, QSpinBox*& _spin, int _lo, int _hi,
                                const QString& _suffix, int _step) {
        _on = new QCheckBox(runPage);
        _spin = new QSpinBox(runPage);
        _spin->setFixedHeight(24);
        _spin->setRange(_lo, _hi);
        _spin->setSingleStep(_step);
        _spin->setSuffix(_suffix);
        auto* row = new QWidget(runPage);
        auto* lay = new QHBoxLayout(row);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->addWidget(_on);
        lay->addWidget(_spin, 1);
        return row;
    };
    runForm->addRow("帧间隔", toggle_row(m_frameMsOn, m_frameMs,
                                         static_cast<int>(proto::config::kFrameMsMin),
                                         static_cast<int>(proto::config::kFrameMsMax), " ms", 8));
    runForm->addRow("帧上限", toggle_row(m_frameBytesOn, m_frameBytes,
                                         static_cast<int>(proto::config::kFrameBytesMin / 1024),
                                         static_cast<int>(proto::config::kFrameBytesMax / 1024), " KB", 64));
    runForm->addRow("单条裁剪", toggle_row(m_trimBytesOn, m_trimBytes,
                                           static_cast<int>(proto::config::kTrimBytesMin / 1024),
                                           static_cast<int>(proto::config::kTrimBytesMax / 1024), " KB", 16));
    m_logFile = new QCheckBox(runPage);
    runForm->addRow("写入日志文件", m_logFile);
    m_frameHint = new QLabel(runPage);
    m_frameHint->setWordWrap(true);
    runForm->addRow(QString(), m_frameHint);

    connect(m_frameMsOn, &QCheckBox::toggled, m_frameMs, &QWidget::setEnabled);
    connect(m_frameBytesOn, &QCheckBox::toggled, m_frameBytes, &QWidget::setEnabled);
    connect(m_trimBytesOn, &QCheckBox::toggled, m_trimBytes, &QWidget::setEnabled);
    for (QCheckBox* on : {m_frameMsOn, m_frameBytesOn, m_trimBytesOn})
        connect(on, &QCheckBox::toggled, this, [this] { update_frame_hint(); });
    for (QSpinBox* spin : {m_frameMs, m_frameBytes, m_trimBytes})
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] { update_frame_hint(); });
    runForm->addItem(new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding));
    ui->stackedWidget->addWidget(runPage);

    // ── page 5: library dirs ──
    auto* libPage = new QWidget(ui->stackedWidget);
    auto* libLay = new QVBoxLayout(libPage);
    libLay->setContentsMargins(12, 12, 12, 12);
    libLay->addWidget(new QLabel("系统库目录（只读，宿主提供）", libPage));
    m_sysLibs = new QListWidget(libPage);
    m_sysLibs->setSelectionMode(QAbstractItemView::NoSelection);
    m_sysLibs->setFocusPolicy(Qt::NoFocus);
    m_sysLibs->setFixedHeight(78);
    libLay->addWidget(m_sysLibs);
    libLay->addWidget(new QLabel("用户库目录（绝对 / 相对 / {script} / {exe}）", libPage));
    m_userLibs = new QListWidget(libPage);
    m_userLibs->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    libLay->addWidget(m_userLibs, 1);
    auto* libBtnRow = new QHBoxLayout;
    auto* addLib = new QPushButton("添加…", libPage);
    addLib->setFixedHeight(24);
    auto* rmLib = new QPushButton("移除", libPage);
    rmLib->setFixedHeight(24);
    libBtnRow->addWidget(addLib);
    libBtnRow->addWidget(rmLib);
    libBtnRow->addStretch();
    libLay->addLayout(libBtnRow);
    connect(addLib, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString text = QInputDialog::getText(
            this, "添加库目录", "路径（绝对、相对、{script}/… 或 {exe}/…）", QLineEdit::Normal,
            QString(), &ok);
        if (ok && !text.trimmed().isEmpty()) m_userLibs->addItem(text.trimmed());
    });
    connect(rmLib, &QPushButton::clicked, this, [this] {
        delete m_userLibs->takeItem(m_userLibs->currentRow());
    });
    ui->stackedWidget->addWidget(libPage);
}

void QScriptSetting::loadValues() {
    m_fontBox->setCurrentText(Settings::editorFont());
    if (m_fontBox->currentIndex() < 0) { // saved family not in the list — offer it anyway
        m_fontBox->addItem(Settings::editorFont());
        m_fontBox->setCurrentIndex(m_fontBox->count() - 1);
    }
    m_fontSize->setValue(Settings::editorFontSize());
    m_uiScale->setCurrentIndex(qMax(0, m_uiScale->findData(Settings::uiScalePercent())));
    m_outputSize->setValue(Settings::outputFontSize());
    m_cbEnabled->setChecked(Settings::completionEnabled());
    m_cbDelay->setValue(Settings::completionDelay());
    m_cbWindowItems->setValue(Settings::completionWindowItems());
    m_cbListLimit->setValue(Settings::completionListLimit());
    m_cbCacheLimit->setValue(Settings::completionCacheLimit());
    m_cbShowDesc->setChecked(Settings::completionShowDesc());
    m_completionSize->setValue(Settings::completionFontSize());
    m_cbHover->setChecked(Settings::hoverEnabled());
    m_hoverSize->setValue(Settings::hoverFontSize());
    m_runnerEnabled->setChecked(Settings::runnerEnabled());
    m_runnerPath->setText(Settings::runnerPath());
    m_runWall->setValue(static_cast<int>(Settings::runWallMs()));
    m_runMem->setValue(static_cast<int>(Settings::runMemBytes() / (1024ull * 1024ull)));
    m_debugOn->setChecked(Settings::debugOnRun());
    m_mtDebug->setChecked(Settings::multiThreadDebug());
    m_logKeep->setValue(Settings::logKeepDays());
    const alx::uint_64 frame_ms = Settings::runFrameMs();
    m_frameMsOn->setChecked(frame_ms != 0);
    m_frameMs->setValue(frame_ms ? static_cast<int>(frame_ms)
                                 : static_cast<int>(proto::config::kFrameMsDefault));
    m_frameMs->setEnabled(frame_ms != 0);
    const alx::uint_64 frame_bytes = Settings::runFrameBytes();
    m_frameBytesOn->setChecked(frame_bytes != 0);
    m_frameBytes->setValue(frame_bytes ? static_cast<int>(frame_bytes / 1024)
                                       : static_cast<int>(proto::config::kFrameBytesDefault / 1024));
    m_frameBytes->setEnabled(frame_bytes != 0);
    const alx::uint_64 trim_bytes = Settings::runTrimBytes();
    m_trimBytesOn->setChecked(trim_bytes != 0);
    m_trimBytes->setValue(trim_bytes ? static_cast<int>(trim_bytes / 1024)
                                     : static_cast<int>(proto::config::kTrimBytesDefault / 1024));
    m_trimBytes->setEnabled(trim_bytes != 0);
    m_logFile->setChecked(Settings::runLogFile());

    m_sysLibs->clear();
    for (const QString& p : Settings::systemLibraryDirs()) m_sysLibs->addItem(p);
    m_userLibs->clear();
    for (const QString& p : Settings::userLibraryDirs()) m_userLibs->addItem(p);
    update_frame_hint();
    populateShortcutTable(m_searchEdit->text().trimmed());
}

void QScriptSetting::saveValues() {
    QSettings s(QStringLiteral("AScript"), QStringLiteral("AScript"));
    s.setValue("editor/fontFamily", m_fontBox->currentText());
    s.setValue("editor/fontSize", m_fontSize->value());
    Settings::setUiScalePercent(m_uiScale->currentData().toInt());
    s.setValue("output/fontSize", m_outputSize->value());
    s.setValue("completion/enabled", m_cbEnabled->isChecked());
    s.setValue("completion/delay", m_cbDelay->value());
    s.setValue("completion/windowItems", m_cbWindowItems->value());
    s.setValue("completion/listLimit", m_cbListLimit->value());
    Settings::setCompletionCacheLimit(m_cbCacheLimit->value());
    s.setValue("completion/showDesc", m_cbShowDesc->isChecked());
    s.setValue("completion/fontSize", m_completionSize->value());
    s.setValue("hover/enabled", m_cbHover->isChecked());
    s.setValue("hover/fontSize", m_hoverSize->value());
    Settings::setRunnerEnabled(m_runnerEnabled->isChecked());
    Settings::setRunnerPath(m_runnerPath->text());
    Settings::setRunWallMs(static_cast<alx::uint_32>(m_runWall->value()));
    Settings::setRunMemBytes(static_cast<alx::uint_64>(m_runMem->value()) * 1024ull * 1024ull);
    Settings::setDebugOnRun(m_debugOn->isChecked());
    Settings::setMultiThreadDebug(m_mtDebug->isChecked());
    Settings::setLogKeepDays(m_logKeep->value());
    Settings::setRunFrameMs(m_frameMsOn->isChecked() ? static_cast<alx::uint_64>(m_frameMs->value()) : 0);
    Settings::setRunFrameBytes(m_frameBytesOn->isChecked()
                                   ? static_cast<alx::uint_64>(m_frameBytes->value()) * 1024ull
                                   : 0);
    Settings::setRunTrimBytes(m_trimBytesOn->isChecked()
                                  ? static_cast<alx::uint_64>(m_trimBytes->value()) * 1024ull
                                  : 0);
    Settings::setRunLogFile(m_logFile->isChecked());
    QStringList userLibs;
    for (int i = 0; i < m_userLibs->count(); ++i) {
        const QString t = m_userLibs->item(i)->text().trimmed();
        if (!t.isEmpty()) userLibs << t;
    }
    Settings::setUserLibraryDirs(userLibs);

    // shortcuts: collect table values, conflict-check, then write
    QList<QPair<QString, QKeySequence>> all;
    for (int row = 0; row < m_shortcutTable->rowCount(); ++row) {
        auto* seqEdit = qobject_cast<QKeySequenceEdit*>(m_shortcutTable->cellWidget(row, 1));
        if (!seqEdit) continue;
        all.append({m_shortcutTable->item(row, 0)->data(Qt::UserRole).toString(), seqEdit->keySequence()});
    }
    QList<QPair<QString, QString>> conflicts;
    if (shortcutConflicts(all, conflicts)) {
        QString msg = "快捷键冲突,请修正后再保存:\n";
        for (const auto& c : conflicts) msg += "  " + c.second + " → " + c.first + "\n";
        QMessageBox::warning(this, "快捷键冲突", msg);
        return;
    }
    for (const auto& p : all)
        s.setValue("shortcut/" + p.first, p.second.toString());
    emit settingsApplied();
}

void QScriptSetting::resetAll() {
    m_fontBox->setCurrentText("Fira Code");
    m_fontSize->setValue(9);
    m_uiScale->setCurrentIndex(0);
    m_outputSize->setValue(9);
    m_cbEnabled->setChecked(true);
    m_cbDelay->setValue(300);
    m_cbWindowItems->setValue(8);
    m_cbListLimit->setValue(32);
    m_cbCacheLimit->setValue(64);
    m_cbShowDesc->setChecked(true);
    m_completionSize->setValue(9);
    m_hoverSize->setValue(9);
    m_runnerEnabled->setChecked(true);
    m_runnerPath->setText(Settings::runnerPath());
    m_runWall->setValue(0);
    m_runMem->setValue(0);
    m_debugOn->setChecked(false);
    m_mtDebug->setChecked(false);
    m_logKeep->setValue(7);
    m_frameMsOn->setChecked(true);
    m_frameMs->setValue(static_cast<int>(proto::config::kFrameMsDefault));
    m_frameMs->setEnabled(true);
    m_frameBytesOn->setChecked(true);
    m_frameBytes->setValue(static_cast<int>(proto::config::kFrameBytesDefault / 1024));
    m_frameBytes->setEnabled(true);
    m_trimBytesOn->setChecked(true);
    m_trimBytes->setValue(static_cast<int>(proto::config::kTrimBytesDefault / 1024));
    m_trimBytes->setEnabled(true);
    m_logFile->setChecked(true);
    m_userLibs->clear();
    update_frame_hint();
    for (const auto& sd : Settings::allShortcuts())
        Settings::resetShortcut(QLatin1String(sd.id));
    m_searchEdit->clear(); // textChanged → table repopulated with defaults
}

void QScriptSetting::update_frame_hint() {
    const int ms = m_frameMsOn->isChecked() ? m_frameMs->value() : 0;
    const int frame_kb = m_frameBytesOn->isChecked() ? m_frameBytes->value() : 0;
    const int trim_kb = m_trimBytesOn->isChecked() ? m_trimBytes->value() : 0;
    QStringList parts;
    if (ms > 0 && frame_kb > 0)
        parts << QStringLiteral("输出上限 ≈ %1 MB/s").arg(frame_kb / static_cast<double>(ms), 0, 'f', 1);
    else if (ms > 0)
        parts << QStringLiteral("帧只在到期时发出，内存峰值 ≈ 速率×T");
    else if (frame_kb > 0)
        parts << QStringLiteral("攒满帧立即发出");
    else
        parts << QStringLiteral("不分帧（回退模式）");
    if (trim_kb > 0 && frame_kb > 0 && trim_kb > frame_kb)
        parts << QStringLiteral("单条裁剪大于帧上限，单笔可能独占超帧");
    m_frameHint->setText(parts.join(QStringLiteral("；")));
}

void QScriptSetting::populateShortcutTable(const QString& _filter) {
    m_shortcutTable->setRowCount(0);
    int row = 0;
    for (const auto& sd : Settings::allShortcuts()) {
        const QString name = QString::fromUtf8(sd.name);
        if (!_filter.isEmpty() && !name.contains(_filter, Qt::CaseInsensitive)) continue;
        m_shortcutTable->insertRow(row);
        auto* nameItem = new QTableWidgetItem(name);
        nameItem->setData(Qt::UserRole, QLatin1String(sd.id));
        m_shortcutTable->setItem(row, 0, nameItem);
        auto* seqEdit = new QKeySequenceEdit(Settings::shortcut(QLatin1String(sd.id)), m_shortcutTable);
        AXC_KEY_SEQUENCE_EDIT_SET_CLEAR_BUTTON(seqEdit);
        m_shortcutTable->setCellWidget(row, 1, seqEdit);
        ++row;
    }
}

bool QScriptSetting::shortcutConflicts(const QList<QPair<QString, QKeySequence>>& _all,
                                       QList<QPair<QString, QString>>& _out) {
    // same-key pairs from (line.cut|line.copy|line.paste) + (edit.cut|edit.copy|
    // edit.paste) are the intentional VSCode context switch — not a conflict
    auto contextPair = [](const QString& _a, const QString& _b) {
        const bool aLine = _a.startsWith("line."), bLine = _b.startsWith("line.");
        const bool aEdit = _a.startsWith("edit."), bEdit = _b.startsWith("edit.");
        return (aLine && bEdit && _a.mid(5) == _b.mid(5)) || (aEdit && bLine && _b.mid(5) == _a.mid(5));
    };
    bool found = false;
    for (int i = 0; i < _all.size(); ++i) {
        if (_all[i].second.isEmpty()) continue; // cleared = unbound
        for (int j = i + 1; j < _all.size(); ++j) {
            if (_all[j].second.isEmpty() || _all[i].second != _all[j].second) continue;
            if (contextPair(_all[i].first, _all[j].first)) continue;
            _out.append({_all[i].first + " / " + _all[j].first, _all[i].second.toString()});
            found = true;
        }
    }
    return found;
}
