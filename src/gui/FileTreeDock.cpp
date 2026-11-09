#include "FileTreeDock.h"

#include "Icons.h"

#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QBrush>
#include <QMessageBox>
#include <QMimeData>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

// ── NameFilterProxy ──

QVariant NameFilterProxy::data(const QModelIndex& _index, int _role) const {
    auto* fs = qobject_cast<QFileSystemModel*>(sourceModel());
    if (fs && _index.column() == 0) {
        const QModelIndex src = mapToSource(_index);
        const bool dir = fs->isDir(src);
        if (_role == Qt::DecorationRole) {
            const QColor color =
                dir ? QColor(0xd7, 0x99, 0x21)
                    : gui::icons::fileColor(QFileInfo(fs->fileName(src)).suffix());
            return gui::icons::load(dir ? QStringLiteral("folder") : QStringLiteral("file"),
                                    color.isValid() ? color : QColor(0xa8, 0x99, 0x84));
        }
        if (_role == Qt::ForegroundRole && !dir) {
            const QColor color = gui::icons::fileColor(QFileInfo(fs->fileName(src)).suffix());
            if (color.isValid()) return QBrush(color);
        }
    }
    return QSortFilterProxyModel::data(_index, _role);
}

bool NameFilterProxy::dropMimeData(const QMimeData* _data, Qt::DropAction, int, int,
                                   const QModelIndex& _parent) {
    auto* fs = static_cast<QFileSystemModel*>(sourceModel());
    const QString destDir = fs->filePath(mapToSource(_parent));
    if (destDir.isEmpty()) return false;
    bool ok = true;
    for (const QUrl& u : _data->urls()) {
        const QString path = u.toLocalFile();
        if (path.isEmpty()) continue;
        const QFileInfo fi(path);
        const QString dest = QDir(destDir).filePath(fi.fileName());
        ok = fi.isDir() ? QDir().rename(path, dest) : QFile::rename(path, dest);
        emit fileMoved(path, dest);
        if (!ok) break;
    }
    return ok;
}

bool NameFilterProxy::filterAcceptsRow(int _row, const QModelIndex& _parent) const {
    const QModelIndex idx = sourceModel()->index(_row, 0, _parent);
    const QString name = sourceModel()->data(idx).toString();
    if (name.contains(m_needle, Qt::CaseInsensitive)) return true;
    if (sourceModel()->hasChildren(idx)) {
        for (int r = 0; r < sourceModel()->rowCount(idx); ++r)
            if (filterAcceptsRow(r, idx)) return true;
    }
    return false;
}

// ── FileTreeDock ──

FileTreeDock::FileTreeDock(QWidget* _parent)
    : QWidget(_parent) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto* header = new QLabel(tr("文件树"), this);
    header->setStyleSheet(QStringLiteral("color:#928374;padding:4px 6px;background:transparent;"));
    lay->addWidget(header);

    m_empty = new QLabel(tr("未打开目录\n使用「文件 → 打开目录」选择文件夹"), this);
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_empty->setStyleSheet(QStringLiteral("color:#928374;background:transparent;"));
    lay->addWidget(m_empty, 1);

    m_searchBox = new QLineEdit(this);
    m_searchBox->setMinimumHeight(24);
    m_searchBox->setPlaceholderText("过滤文件…");
    m_searchBox->setClearButtonEnabled(true);
    lay->addWidget(m_searchBox);

    m_tree = new QTreeView(this);
    m_tree->setHeaderHidden(true);
    lay->addWidget(m_tree, 1);

    m_searchBox->hide();
    m_tree->hide();
    buildTree();
}

void FileTreeDock::buildTree() {
    m_fsModel = new QFileSystemModel(this);
    m_fsModel->setReadOnly(false); // root is set by QScriptMain — never "/" by default

    m_proxy = new NameFilterProxy(this);
    m_proxy->setSourceModel(m_fsModel);
    m_tree->setModel(m_proxy);
    m_tree->setIconSize(QSize(16, 16));
    m_tree->setUniformRowHeights(true);
    m_tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_tree->hideColumn(1); // size
    m_tree->hideColumn(2); // type
    m_tree->hideColumn(3); // date

    // 150ms debounce: filtering walks the whole subtree on every keystroke
    m_filterTimer = new QTimer(this);
    m_filterTimer->setSingleShot(true);
    m_filterTimer->setInterval(150);
    connect(m_searchBox, &QLineEdit::textChanged, m_filterTimer, qOverload<>(&QTimer::start));
    connect(m_filterTimer, &QTimer::timeout, this, [this] { applyFilter(m_searchBox->text()); });
    connect(m_fsModel, &QFileSystemModel::directoryLoaded, this, [this](const QString& _dir) {
        m_proxy->refilter();
        if (!m_searchBox->text().isEmpty()) expandAllDirs(m_fsModel->index(_dir));
    });

    m_tree->setEditTriggers(QAbstractItemView::EditKeyPressed);
    connect(m_tree, &QTreeView::doubleClicked, this, [this](const QModelIndex& _idx) {
        const QModelIndex src = m_proxy->mapToSource(_idx);
        if (m_fsModel->isDir(src)) return;
        const QString path = m_fsModel->filePath(src);
        const QString suf = QFileInfo(path).suffix();
        if (suf == "ax")
            emit axSelected(path);
        else if (suf == "axc" || suf == "json" || suf == "txt" || suf == "md")
            emit fileOpened(path);
        else if (suf == "axp")
            emit axpSelected(path);
    });

    m_tree->setDragEnabled(true);
    m_tree->setAcceptDrops(true);
    m_tree->setDropIndicatorShown(true);
    m_tree->setDragDropMode(QAbstractItemView::DragDrop);
    m_tree->setDefaultDropAction(Qt::MoveAction);

    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeView::customContextMenuRequested, this, &FileTreeDock::fileTreeMenu);

    connect(m_proxy, &NameFilterProxy::fileMoved, this, [this](const QString& _from, const QString& _to) {
        emit logMessage("移动: " + _from + " → " + _to);
    });
    connect(m_fsModel, &QFileSystemModel::fileRenamed, this,
            [this](const QString&, const QString& _old, const QString& _new) {
                emit logMessage("重命名: " + _old + " → " + _new);
            });
}

void FileTreeDock::applyFilter(const QString& _text) {
    m_proxy->setNeedle(_text);
    m_tree->setDragEnabled(_text.isEmpty());
    if (m_lastDir.isEmpty()) return;
    m_tree->setRootIndex(m_proxy->mapFromSource(m_fsModel->index(m_lastDir)));
    if (!_text.isEmpty()) expandAllDirs(m_fsModel->index(m_lastDir));
}

void FileTreeDock::setRootPath(const QString& _path) {
    if (!QFileInfo(_path).isDir()) return;
    m_fsModel->setRootPath(_path);
    m_lastDir = _path;
    m_searchBox->clear(); // a new root starts unfiltered
    m_filterTimer->stop();
    applyFilter(QString()); // reset the proxy before anything maps through it
    m_empty->hide();
    m_searchBox->show();
    m_tree->show();
    emit logMessage("打开目录: " + _path);
}

void FileTreeDock::clearRoot() {
    m_searchBox->clear();
    m_filterTimer->stop();
    m_proxy->setNeedle(QString());
    m_tree->setDragEnabled(true);
    m_lastDir.clear();
    m_tree->setRootIndex(QModelIndex());
    m_tree->hide();
    m_searchBox->hide();
    m_empty->show();
}

void FileTreeDock::refreshModel() {
    if (!m_lastDir.isEmpty())
        m_fsModel->fetchMore(m_fsModel->index(m_lastDir));
}

void FileTreeDock::selectFile(const QString& _path) {
    if (_path.isEmpty() || m_lastDir.isEmpty()) return;
    // .axp embed modules use "path.axp@key" — strip the @ suffix for tree lookup
    QString filePath = _path;
    const int at = filePath.lastIndexOf('@');
    if (at > 0) filePath.truncate(at);
    const QModelIndex srcIdx = m_fsModel->index(filePath);
    if (!srcIdx.isValid()) return;
    const QModelIndex proxyIdx = m_proxy->mapFromSource(srcIdx);
    if (!proxyIdx.isValid()) return;
    // expand parent chain so the item is visible
    QModelIndex parent = proxyIdx.parent();
    while (parent.isValid()) {
        m_tree->expand(parent);
        parent = parent.parent();
    }
    m_tree->setCurrentIndex(proxyIdx);
    m_tree->scrollTo(proxyIdx);
}

void FileTreeDock::expandAllDirs(const QModelIndex& _root) {
    for (int r = 0; r < m_fsModel->rowCount(_root); ++r) {
        const QModelIndex child = m_fsModel->index(r, 0, _root);
        if (!m_fsModel->isDir(child)) continue;
        m_fsModel->fetchMore(child);
        const QModelIndex proxyChild = m_proxy->mapFromSource(child);
        if (proxyChild.isValid()) m_tree->expand(proxyChild);
        expandAllDirs(child);
    }
}

void FileTreeDock::fileTreeMenu(const QPoint& _pos) {
    const QModelIndex src = m_proxy->mapToSource(m_tree->indexAt(_pos));
    QString baseDir = src.isValid() ? m_fsModel->filePath(src) : QString();
    if (src.isValid() && !m_fsModel->isDir(src)) baseDir = QFileInfo(baseDir).path();
    if (baseDir.isEmpty()) baseDir = m_fsModel->rootPath();
    if (baseDir.isEmpty()) return;
    const QDir dir(baseDir);

    QMenu menu(this);
    QAction* newFile = menu.addAction("新建文件");
    QAction* newDir = menu.addAction("新建文件夹");
    const bool onFileNode = src.isValid() && !m_fsModel->isDir(src);
    QAction* newAx = onFileNode ? nullptr : menu.addAction("新建 .ax 配置…");
    menu.addSeparator();
    QAction* rename = src.isValid() ? menu.addAction("重命名") : nullptr;
    QAction* del = src.isValid() ? menu.addAction("删除") : nullptr;
    menu.addSeparator();
    const QString ext = onFileNode ? QFileInfo(m_fsModel->filePath(src)).suffix() : QString();
    const bool isAx = ext == "ax";
    const bool canRun = onFileNode && (isAx || ext == "axc" || ext == "axp");
    QAction* axSet = isAx ? menu.addAction("设为工程") : nullptr;
    QAction* axRun = canRun ? menu.addAction("运行") : nullptr;
    QAction* axRunDebug = canRun ? menu.addAction("调试运行") : nullptr;
    QAction* axConf = isAx ? menu.addAction("配置(.ax 表单)…") : nullptr;
    const bool canCompile = onFileNode && (isAx || ext == "axc");
    QAction* compile = canCompile ? menu.addAction("编译…") : nullptr;
    QAction* compileTree =
        (!src.isValid() && !m_lastDir.isEmpty()) ? menu.addAction("编译文件树…") : nullptr;
    QAction* props = menu.addAction("属性");
    QAction* copyPath = src.isValid() ? menu.addAction("复制路径") : nullptr;
    QAction* picked = menu.exec(m_tree->viewport()->mapToGlobal(_pos));
    if (!picked) return;

    const QString filePath = src.isValid() ? m_fsModel->filePath(src) : QString();

    if (picked == newAx) {
        QInputDialog dlg(this);
        dlg.setWindowTitle("新建 .ax 配置");
        dlg.setLabelText("文件名:");
        dlg.setTextValue("新工程.ax");
        if (auto* le = dlg.findChild<QLineEdit*>()) le->setSelection(0, 3);
        if (dlg.exec() != QDialog::Accepted) return;
        const QString name = dlg.textValue().trimmed();
        if (name.isEmpty()) return;
        const QString full = name.endsWith(".ax") ? name : name + ".ax";
        const QString path = QDir(baseDir).filePath(full);
        if (QFileInfo::exists(path)) {
            QMessageBox::warning(this, "新建 .ax 配置", "已存在: " + full);
            return;
        }
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            QMessageBox::warning(this, "新建 .ax 配置", "无法创建 " + full);
        else {
            emit logMessage("新建 .ax 配置: " + path);
            m_fsModel->fetchMore(m_fsModel->index(baseDir));
            emit axConfigRequested(path); // open the form for the empty file
        }
    } else if (picked == axSet) {
        emit axSelected(filePath);
    } else if (picked == axRun) {
        emit runRequested(filePath);
    } else if (picked == axRunDebug) {
        emit runDebugRequested(filePath);
    } else if (picked == axConf) {
        emit axConfigRequested(filePath);
    } else if (picked == compile) {
        emit compileRequested(filePath);
    } else if (picked == compileTree) {
        emit compileTreeRequested();
    } else if (picked == props) {
        emit propsRequested(src.isValid() ? filePath : baseDir);
    } else if (picked == copyPath) {
        QGuiApplication::clipboard()->setText(filePath);
    } else if (picked == newFile) {
        bool ok = false;
        const QString name = QInputDialog::getText(this, "新建文件", "文件名:", QLineEdit::Normal, QString(), &ok);
        if (!ok || name.isEmpty()) return;
        const QString path = dir.filePath(name);
        if (QFileInfo::exists(path)) {
            QMessageBox::warning(this, "新建文件", "已存在: " + name);
            return;
        }
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            QMessageBox::warning(this, "新建文件", "无法创建 " + name);
        else {
            emit logMessage("新建文件: " + path);
            m_fsModel->fetchMore(m_fsModel->index(dir.path()));
        }
    } else if (picked == newDir) {
        bool ok = false;
        const QString name = QInputDialog::getText(this, "新建文件夹", "文件夹名:", QLineEdit::Normal, QString(), &ok);
        if (!ok || name.isEmpty()) return;
        if (QFileInfo::exists(dir.filePath(name))) {
            QMessageBox::warning(this, "新建文件夹", "已存在: " + name);
            return;
        }
        if (!dir.mkdir(name))
            QMessageBox::warning(this, "新建文件夹", "无法创建 " + name);
        else {
            emit logMessage("新建文件夹: " + dir.filePath(name));
            m_fsModel->fetchMore(m_fsModel->index(dir.path()));
        }
    } else if (picked == rename) {
        m_tree->edit(m_proxy->mapFromSource(src));
    } else if (picked == del) {
        const QString path = m_fsModel->filePath(src);
        if (QMessageBox::question(this, "删除", "确定删除 " + path + " ?") != QMessageBox::Yes) return;
        const bool ok = m_fsModel->isDir(src) ? QDir(path).removeRecursively() : QFile::remove(path);
        if (!ok)
            QMessageBox::warning(this, "删除", "无法删除 " + path);
        else
            emit logMessage("删除: " + path);
    }
}
