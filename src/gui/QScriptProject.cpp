#include "QScriptProject.h"
#include "ui_QScriptProject.h"

#include "Icons.h"
#include "core/project.h"

#include <QColor>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
#include <QShowEvent>
#include <QToolButton>

namespace {

    bool isPathPlaceholder(const QListWidgetItem* _item) {
        return _item && _item->data(Qt::UserRole).toBool();
    }

    // MaskWidget — dim overlay blocking the parent while the panel is open
    // (lock-window style, QScriptSetting pattern): the parent stays enabled, so
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

QScriptProject::QScriptProject(QWidget* parent, const QString& _axPath)
    : EmbedWidget(parent), ui(new Ui::QScriptProject), m_axPath(_axPath) {
    ui->setupUi(this);
    QFont tf = ui->statusLabel->font();
    tf.setBold(true);
    ui->statusLabel->setFont(tf);
    ui->closeBtn->setText(QString());
    ui->closeBtn->setIcon(gui::icons::load(QStringLiteral("close")));
    ui->closeBtn->setIconSize(QSize(14, 14));
    ui->btnSave->setProperty("primary", true);
    connect(ui->closeBtn, &QToolButton::clicked, this, [this] { hide(); });
    connect(ui->btnSave, &QPushButton::clicked, this, [this] { saveValues(); });
    connect(ui->btnCancel, &QPushButton::clicked, this, [this] { hide(); });
    connect(ui->addPathBtn, &QPushButton::clicked, this, [this] {
        const QDir base(QFileInfo(m_axPath).absolutePath());
        const QString dir = QFileDialog::getExistingDirectory(
            this, "添加搜索路径", base.exists() ? base.absolutePath() : QString());
        if (dir.isEmpty()) return;
        const QString rel = base.exists() ? base.relativeFilePath(dir) : dir;
        if (ui->pathList->findItems(rel, Qt::MatchExactly).isEmpty()) {
            if (ui->pathList->count() == 1 && isPathPlaceholder(ui->pathList->item(0)))
                delete ui->pathList->takeItem(0); // drop the grey placeholder
            ui->pathList->addItem(rel);
        }
    });
    connect(ui->delPathBtn, &QPushButton::clicked, this, [this] {
        for (QListWidgetItem* it : ui->pathList->selectedItems()) delete it;
        updatePathPlaceholder(ui->pathList);
    });
    connect(ui->addDocPathBtn, &QPushButton::clicked, this, [this] {
        const QDir base(QFileInfo(m_axPath).absolutePath());
        const QString dir = QFileDialog::getExistingDirectory(
            this, "添加提示目录", base.exists() ? base.absolutePath() : QString());
        if (dir.isEmpty()) return;
        const QString rel = base.exists() ? base.relativeFilePath(dir) : dir;
        if (ui->docPathList->findItems(rel, Qt::MatchExactly).isEmpty()) {
            if (ui->docPathList->count() == 1 && isPathPlaceholder(ui->docPathList->item(0)))
                delete ui->docPathList->takeItem(0);
            ui->docPathList->addItem(rel);
        }
    });
    connect(ui->delDocPathBtn, &QPushButton::clicked, this, [this] {
        for (QListWidgetItem* it : ui->docPathList->selectedItems()) delete it;
        updatePathPlaceholder(ui->docPathList);
    });
    connect(ui->browseHintBtn, &QPushButton::clicked, this, [this] {
        const QDir base(QFileInfo(m_axPath).absolutePath());
        const QString f = QFileDialog::getOpenFileName(
            this, "选择 hint JSON", base.exists() ? base.absolutePath() : QString(), "*.json");
        if (f.isEmpty()) return;
        ui->hintEdit->setText(base.exists() ? base.relativeFilePath(f) : f);
    });
    connect(ui->browseOutBtn, &QPushButton::clicked, this, [this] {
        const QDir base(QFileInfo(m_axPath).absolutePath());
        const QString dir = QFileDialog::getExistingDirectory(
            this, "选择输出目录", base.exists() ? base.absolutePath() : QString());
        if (dir.isEmpty()) return;
        ui->outEdit->setText(base.exists() ? base.relativeFilePath(dir) : dir);
    });
    loadValues();
    // name/out rows sit outside the form layout — align their labels with its column
    const int labelW = ui->maxStackLabel->sizeHint().width();
    ui->nameLabel->setMinimumWidth(labelW);
    ui->outLabel->setMinimumWidth(labelW);
    setMinimumWidth(440);
    adjustSize();
    hide();
}

QScriptProject::~QScriptProject() {
    delete ui;
}

void QScriptProject::showEvent(QShowEvent* _ev) {
    loadValues(); // re-read on every open — external hand-written changes win
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

void QScriptProject::hideEvent(QHideEvent* _ev) {
    // form-created empty .ax cancelled without a save → remove it
    if (m_createdByForm && !m_axPath.isEmpty()) {
        m_createdByForm = false; // consumed once
        const QFileInfo fi(m_axPath);
        if (fi.exists() && fi.size() == 0) QFile::remove(m_axPath);
    }
    if (m_mask) m_mask->hide();
    EmbedWidget::hideEvent(_ev);
}

void QScriptProject::loadValues() {
    populateMainList();
    // defaults (0 = don't write the key; parse_depth default is engine's 1024)
    ui->overflowCheck->setChecked(false);
    ui->maxStackSpin->setValue(0);
    ui->maxVecfillSpin->setValue(0);
    ui->parseDepthSpin->setValue(0);
    ui->pathList->clear();
    ui->docPathList->clear();
    ui->hintEdit->clear();
    ui->nameEdit->clear();
    ui->outEdit->clear();

    if (m_axPath.isEmpty() || QFileInfo(m_axPath).size() == 0) {
        ui->statusLabel->setText("新建 .ax 配置");
        updatePathPlaceholder(ui->pathList);
        updatePathPlaceholder(ui->docPathList);
        return;
    }
    core::project::ax_config ax;
    std::string err;
    if (!core::project::parse_ax(m_axPath.toStdString(), ax, err)) {
        ui->statusLabel->setText("配置解析失败: " + QString::fromStdString(err));
        updatePathPlaceholder(ui->pathList);
        updatePathPlaceholder(ui->docPathList);
        return;
    }
    ui->statusLabel->setText(QFileInfo(m_axPath).fileName());
    ui->nameEdit->setText(QString::fromStdString(ax.name));
    const QDir dir(QString::fromStdString(ax.dir));
    if (!ax.out.empty()) ui->outEdit->setText(dir.relativeFilePath(QString::fromStdString(ax.out)));
    const QString main = dir.relativeFilePath(QString::fromStdString(ax.main));
    ui->mainBox->setCurrentText(main); // editable: saved value may be outside the enum
    if (ax.engine.contain("overflow_check"))
        ui->overflowCheck->setChecked(ax.engine.value("overflow_check").to<bool>(false));
    if (ax.engine.contain("max_stack"))
        ui->maxStackSpin->setValue(static_cast<int>(ax.engine.value("max_stack").to<alx::int_64>(0)));
    if (ax.engine.contain("max_vecfill"))
        ui->maxVecfillSpin->setValue(static_cast<int>(ax.engine.value("max_vecfill").to<alx::int_64>(0)));
    if (ax.engine.contain("parse_depth"))
        ui->parseDepthSpin->setValue(static_cast<int>(ax.engine.value("parse_depth").to<alx::int_64>(0)));
    ui->hookCheck->setChecked(ax.hook);
    ui->hookFreqSpin->setValue(static_cast<int>(ax.hook_freq));
    for (const std::string& p : ax.path)
        ui->pathList->addItem(dir.relativeFilePath(QString::fromStdString(p)));
    for (const std::string& p : ax.docpath)
        ui->docPathList->addItem(dir.relativeFilePath(QString::fromStdString(p)));
    updatePathPlaceholder(ui->pathList);
    updatePathPlaceholder(ui->docPathList);
    if (!ax.hint.empty()) ui->hintEdit->setText(dir.relativeFilePath(QString::fromStdString(ax.hint)));
}

void QScriptProject::updatePathPlaceholder(QListWidget* _list) {
    if (_list->count() > 0) return;
    const bool docs = _list == ui->docPathList;
    auto* item = new QListWidgetItem(docs ? QStringLiteral("尚未添加提示目录") : QStringLiteral("尚未添加搜索路径"), _list);
    item->setFlags(Qt::NoItemFlags);
    item->setData(Qt::UserRole, true); // sentinel: never saved as a path
    item->setForeground(QColor(0x92, 0x83, 0x74));
}

void QScriptProject::saveValues() {
    const QString main = ui->mainBox->currentText().trimmed();
    if (main.isEmpty()) {
        QMessageBox::warning(this, "工程配置", "main 不能为空——请选择起始 axc 脚本");
        return;
    }
    if (m_axPath.isEmpty()) { // defensive: location usually picked before open
        m_axPath = QFileDialog::getSaveFileName(this, "新建 .ax 工程配置", QString(), "*.ax");
        if (m_axPath.isEmpty()) return;
        populateMainList(); // candidates appear once the dir is known
        ui->statusLabel->setText(QFileInfo(m_axPath).fileName());
        return; // pick main and save again
    }
    const QDir dir(QFileInfo(m_axPath).absolutePath());

    // build the JSON — non-default keys only (lean hand-writable files)
    alx::json_object o;
    const QString projectName = ui->nameEdit->text().trimmed();
    if (!projectName.isEmpty()) o["name"] = alx::json_value(projectName.toStdString());
    const QString outDir = ui->outEdit->text().trimmed();
    if (!outDir.isEmpty()) o["out"] = alx::json_value(outDir.toStdString());
    o["main"] = alx::json_value(main.toStdString());
    alx::json_object engine;
    if (ui->overflowCheck->isChecked()) engine["overflow_check"] = alx::json_value(true);
    if (ui->maxStackSpin->value() > 0) engine["max_stack"] = alx::json_value(ui->maxStackSpin->value());
    if (ui->maxVecfillSpin->value() > 0) engine["max_vecfill"] = alx::json_value(ui->maxVecfillSpin->value());
    if (ui->parseDepthSpin->value() > 0) engine["parse_depth"] = alx::json_value(ui->parseDepthSpin->value());
    if (!engine.empty()) o["engine"] = alx::json_value(std::move(engine));
    if (!ui->hookCheck->isChecked()) o["hook"] = alx::json_value(false);
    if (ui->hookFreqSpin->value() != 1) o["hook_freq"] = alx::json_value(ui->hookFreqSpin->value());
    alx::json_array path;
    for (int i = 0; i < ui->pathList->count(); ++i) {
        QListWidgetItem* item = ui->pathList->item(i);
        if (isPathPlaceholder(item)) continue;
        const QString p = item->text().trimmed();
        if (!p.isEmpty()) path.append(alx::json_value(p.toStdString()));
    }
    if (!path.empty()) o["path"] = alx::json_value(std::move(path));
    alx::json_array docpath;
    for (int i = 0; i < ui->docPathList->count(); ++i) {
        QListWidgetItem* item = ui->docPathList->item(i);
        if (isPathPlaceholder(item)) continue;
        const QString p = item->text().trimmed();
        if (!p.isEmpty()) docpath.append(alx::json_value(p.toStdString()));
    }
    if (!docpath.empty()) o["docpath"] = alx::json_value(std::move(docpath));
    const QString hint = ui->hintEdit->text().trimmed();
    if (!hint.isEmpty()) o["hint"] = alx::json_value(hint.toStdString());

    const std::string json = alx::json_doc::to_string(alx::json_value(std::move(o)), false);
    QFile f(m_axPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, "工程配置", "无法写入 " + m_axPath);
        return;
    }
    f.write(json.data(), static_cast<qint64>(json.size()));
    f.close(); // saved handlers parse the file immediately
    emit saved(m_axPath);
    hide();
}

void QScriptProject::populateMainList() {
    ui->mainBox->clear();
    const QFileInfo fi(m_axPath);
    if (fi.absolutePath().isEmpty()) return; // no location picked yet
    const QDir dir(fi.absolutePath());
    if (!dir.exists()) return; // the .ax may not exist yet — the dir drives the enum
    QDirIterator it(dir.absolutePath(), QStringList() << "*.axc", QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) ui->mainBox->addItem(dir.relativeFilePath(it.next()));
}
