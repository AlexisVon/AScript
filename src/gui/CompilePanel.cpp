#include "CompilePanel.h"

#include "Settings.h"
#include "core/project.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyle>
#include <QTextDocument>
#include <QVBoxLayout>

CompilePanel::CompilePanel(QWidget* parent, const QString& _dir, const QString& _projectAx,
                           const QString& _projectName, const QString& _currentFile,
                           const std::function<bool(const QString&)>& _isDirty,
                           const QString& _selectedFile, bool _treeFirst)
    : EmbedWidget(parent),
      m_dir(_dir),
      m_projectAx(_projectAx),
      m_projectName(_projectName),
      m_currentFile(_currentFile),
      m_selectedFile(_selectedFile),
      m_isDirty(_isDirty),
      m_treeFirst(_treeFirst) {
    buildUi();
}

void CompilePanel::buildUi() {
    setFixedWidth(500);

    auto* titleRow = gui::titleRow(tr("编译"), this, [this] { hide(); });

    m_scope = new QComboBox(this);
    m_scope->setFixedHeight(24);
    const QString file = m_selectedFile.isEmpty() ? m_currentFile : m_selectedFile;
    const QString suffix = QFileInfo(file).suffix();
    const bool fileOk = !file.isEmpty() && (suffix == "ax" || suffix == "axc");
    if (fileOk) m_scope->addItem(tr("文件: ") + QFileInfo(file).fileName(), int(ScopeFile));
    if (!m_projectAx.isEmpty())
        m_scope->addItem(tr("工程: ") + (m_projectName.isEmpty() ? QFileInfo(m_projectAx).completeBaseName()
                                                                 : m_projectName),
                         int(ScopeProject));
    if (!m_dir.isEmpty()) m_scope->addItem(tr("文件树全部 .axc"), int(ScopeTree));
    if (m_scope->count() == 0) m_scope->addItem(tr("无可编译目标"));

    int want = ScopeFile;
    if (m_treeFirst) want = m_projectAx.isEmpty() ? ScopeTree : ScopeProject;
    else if (!fileOk) want = m_projectAx.isEmpty() ? ScopeTree : ScopeProject;
    const int idx = m_scope->findData(int(want));
    m_scope->setCurrentIndex(idx >= 0 ? idx : 0);

    auto* dirRow = new QWidget(this);
    auto* dirLay = new QHBoxLayout(dirRow);
    dirLay->setContentsMargins(0, 0, 0, 0);
    dirLay->setSpacing(4);
    m_outDir = new QLineEdit(dirRow);
    m_outDir->setFixedHeight(24);
    m_outDir->setPlaceholderText(tr("未配置，请选择"));
    m_outDir->setText(configuredOutDir());
    auto* browse = new QPushButton(tr("浏览…"), dirRow);
    browse->setFixedSize(64, 24);
    dirLay->addWidget(m_outDir, 1);
    dirLay->addWidget(browse);

    auto* optRow = new QWidget(this);
    auto* optLay = new QHBoxLayout(optRow);
    optLay->setContentsMargins(0, 0, 0, 0);
    optLay->setSpacing(12);
    m_debug = new QCheckBox(tr("调试信息"), optRow);
    m_debug->setToolTip(tr("在产物中写入语句位置（debug_enable）"));
    m_debug->setChecked(Settings::compileDebug());
    m_embed = new QCheckBox(tr("embed 闭包"), optRow);
    m_embed->setToolTip(tr("import 全部内嵌；link/env 变为编译错误"));
    m_embed->setChecked(Settings::compileEmbed());
    optLay->addWidget(m_debug);
    optLay->addWidget(m_embed);
    optLay->addStretch();

    auto* form = new QFormLayout;
    form->setHorizontalSpacing(8);
    form->setVerticalSpacing(8);
    form->addRow(tr("范围"), m_scope);
    form->addRow(tr("输出目录"), dirRow);
    form->addRow(tr("选项"), optRow);

    m_result = new QPlainTextEdit(this);
    m_result->setObjectName(QStringLiteral("compileResult"));
    m_result->setReadOnly(true);
    m_result->setFixedHeight(120);
    m_result->setFont(QFont(Settings::editorFont(), Settings::outputFontSize()));
    m_result->document()->setDocumentMargin(6);
    m_result->setPlainText(tr("输出目录必选；编译结果会显示在这里。"));

    m_runBtn = new QPushButton(tr("编译"), this);
    m_runBtn->setProperty("primary", true);
    m_runBtn->setFixedSize(72, 26);
    if (m_scope->currentData().isValid() == false) m_runBtn->setEnabled(false);
    auto* closeBtn = new QPushButton(tr("关闭"), this);
    closeBtn->setFixedSize(64, 26);

    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch();
    btnRow->addWidget(m_runBtn);
    btnRow->addWidget(closeBtn);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 8, 10, 10);
    lay->setSpacing(8);
    lay->addLayout(titleRow);
    lay->addLayout(form);
    lay->addWidget(m_result);
    lay->addLayout(btnRow);

    connect(browse, &QPushButton::clicked, this, [this] {
        const QString dir =
            QFileDialog::getExistingDirectory(this, tr("选择输出目录"), m_outDir->text());
        if (dir.isEmpty()) return;
        m_outDir->setText(dir);
        m_outAuto = false; // a browsed directory survives scope changes
    });
    connect(m_outDir, &QLineEdit::textEdited, this, [this] { m_outAuto = false; });
    connect(m_scope, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        if (m_outAuto) m_outDir->setText(configuredOutDir());
    });
    connect(m_runBtn, &QPushButton::clicked, this, &CompilePanel::runCompile);
    connect(closeBtn, &QPushButton::clicked, this, [this] { hide(); });
}

void CompilePanel::runCompile() {
    if (!m_scope->currentData().isValid()) {
        setResult(tr("没有可编译的目标"), true);
        return;
    }
    const QString outDir = m_outDir->text().trimmed();
    if (outDir.isEmpty()) {
        setResult(tr("请选择输出目录"), true);
        return;
    }
    if (!QFileInfo(outDir).isDir() && !QDir().mkpath(outDir)) {
        setResult(tr("无法创建输出目录: ") + outDir, true);
        return;
    }
    const bool debug = m_debug->isChecked();
    const bool embed = m_embed->isChecked();
    Settings::setCompileDebug(debug);
    Settings::setCompileEmbed(embed);

    const int scope = m_scope->currentData().toInt();
    const QString target = m_selectedFile.isEmpty() ? m_currentFile : m_selectedFile;
    QStringList lines;
    QStringList dirty;
    int ok = 0, fail = 0;
    auto relName = [this](const QString& _file) {
        return m_dir.isEmpty() ? QFileInfo(_file).fileName() : QDir(m_dir).relativeFilePath(_file);
    };
    auto okLine = [&](const QString& _file, const QString& _out) {
        ++ok;
        lines.append(QStringLiteral("ok   %1 → %2").arg(relName(_file), _out));
    };
    auto failLine = [&](const QString& _file, const QString& _err) {
        ++fail;
        lines.append(QStringLiteral("FAIL %1: %2").arg(relName(_file), _err));
    };

    core::project::ax_config proj;
    bool hasProj = false;
    std::string perr;
    if (!m_projectAx.isEmpty())
        hasProj = core::project::parse_ax(m_projectAx.toStdString(), proj, perr);
    auto insideProj = [&](const QString& _file) {
        if (!hasProj) return false;
        const QString rel = QDir(QString::fromStdString(proj.dir)).relativeFilePath(_file);
        return !rel.startsWith(QLatin1String("..")) && !QDir::isAbsolutePath(rel);
    };

    auto compileAx = [&](const QString& _axFile) {
        const QString abs = QFileInfo(_axFile).absoluteFilePath();
        core::project::ax_config ax;
        std::string err;
        if (!core::project::parse_ax(abs.toStdString(), ax, err)) {
            failLine(abs, QString::fromStdString(err));
            return;
        }
        const QString out = QDir(outDir).filePath(QFileInfo(abs).completeBaseName() + ".axp");
        std::vector<std::string> extra;
        for (const QString& p : Settings::userLibraryDirs()) extra.push_back(p.toStdString());
        if (!core::project::compile(ax, embed, out.toStdString(), err, true, debug ? 1 : 0, extra))
            failLine(abs, QString::fromStdString(err));
        else
            okLine(abs, out);
    };
    auto compileScript = [&](const QString& _file, const QString& _out) {
        const QString abs = QFileInfo(_file).absoluteFilePath();
        if (m_isDirty && m_isDirty(abs)) {
            const QString name = relName(abs);
            if (!dirty.contains(name)) dirty.append(name);
        }
        std::string err;
        const core::project::ax_config* p = insideProj(abs) ? &proj : nullptr;
        std::vector<std::string> extra;
        for (const QString& q : Settings::userLibraryDirs()) extra.push_back(q.toStdString());
        if (!core::project::compile_file(abs.toStdString(), _out.toStdString(), debug, embed, p, err,
                                         true, extra))
            failLine(abs, QString::fromStdString(err));
        else
            okLine(abs, _out);
    };

    if (scope == ScopeFile) {
        const QString abs = QFileInfo(target).absoluteFilePath();
        const QString sfx = QFileInfo(abs).suffix();
        if (sfx == "ax") {
            compileAx(abs);
        } else if (sfx == "axc") {
            const QString out = QDir(outDir).filePath(QFileInfo(abs).completeBaseName() + ".axp");
            compileScript(abs, out);
        } else {
            failLine(abs, tr("不是 .ax/.axc"));
        }
    } else if (scope == ScopeProject) {
        if (m_projectAx.isEmpty()) failLine(m_projectAx, tr("没有工程"));
        else compileAx(m_projectAx);
    } else if (m_dir.isEmpty()) {
        setResult(tr("没有打开文件夹"), true);
        return;
    } else {
        QStringList files;
        QDirIterator it(m_dir, QStringList() << "*.axc", QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) files.append(it.next());
        files.sort();
        if (files.isEmpty()) {
            setResult(tr("文件树中没有 .axc"), true);
            return;
        }
        for (const QString& file : files) {
            QString outName = QDir(m_dir).relativeFilePath(file);
            outName.chop(4); // .axc
            outName += ".axp";
            const QString out = QDir(outDir).filePath(outName);
            QDir().mkpath(QFileInfo(out).absolutePath());
            compileScript(file, out);
        }
    }

    QString text = lines.join('\n');
    if (!dirty.isEmpty())
        text.prepend(tr("注意: 以下文件有未保存修改，编译使用磁盘版本:\n  ") +
                     dirty.join(QStringLiteral("\n  ")) + QStringLiteral("\n\n"));
    text += QStringLiteral("\n\n") + tr("成功 %1 / 失败 %2").arg(ok).arg(fail);
    setResult(text, fail > 0);
    emit logMessage(tr("编译: 成功 %1 / 失败 %2").arg(ok).arg(fail));
}

QString CompilePanel::configuredOutDir() const {
    const int scope = m_scope->currentData().toInt();
    const QString file = m_selectedFile.isEmpty() ? m_currentFile : m_selectedFile;
    const QString fileSuffix = QFileInfo(file).suffix();
    QString axPath;
    if (scope == ScopeProject || scope == ScopeTree) axPath = m_projectAx;
    else if (fileSuffix == "ax") axPath = file;
    else axPath = m_projectAx; // .axc: the project's out applies when inside the project
    if (axPath.isEmpty()) return QString();

    core::project::ax_config ax;
    std::string err;
    if (!core::project::parse_ax(axPath.toStdString(), ax, err)) return QString();
    if (scope == ScopeFile && fileSuffix != "ax") {
        const QString rel =
            QDir(QString::fromStdString(ax.dir)).relativeFilePath(QFileInfo(file).absoluteFilePath());
        if (rel.startsWith(QLatin1String("..")) || QDir::isAbsolutePath(rel)) return QString();
    }
    return QString::fromStdString(ax.out); // empty = not configured
}

void CompilePanel::setResult(const QString& _text, bool _error) {
    m_result->setProperty("state", _error ? QStringLiteral("error") : QStringLiteral("ok"));
    m_result->style()->unpolish(m_result);
    m_result->style()->polish(m_result);
    m_result->setPlainText(_text);
}
