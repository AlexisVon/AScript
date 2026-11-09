#include "OpenFilesPanel.h"

#include "Icons.h"

#include <QClipboard>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QVBoxLayout>

namespace {
    // QListWidget's 71px minimum kept this section from shrinking to its content
    class OpenFileList : public QListWidget {
    public:
        using QListWidget::QListWidget;
        QSize minimumSizeHint() const override { return {0, 24}; }
    };
} // namespace

OpenFilesPanel::OpenFilesPanel(QWidget* parent)
    : QWidget(parent) {
    m_header = new QLabel(tr("打开的文件"), this);
    m_header->setStyleSheet(
        QStringLiteral("color:#928374;padding:4px 6px;background:transparent;"));

    m_list = new OpenFileList(this);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setUniformItemSizes(true);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(m_header);
    lay->addWidget(m_list, 1);

    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem* _item) {
        if (_item) emit activateRequested(_item->data(Qt::UserRole).toInt());
    });
    connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint& _pos) {
        QListWidgetItem* item = m_list->itemAt(_pos);
        if (!item) return;
        const int index = item->data(Qt::UserRole).toInt();
        const QString path = item->data(Qt::UserRole + 1).toString();
        const bool readOnly = item->data(Qt::UserRole + 2).toBool();
        const QString suffix = QFileInfo(path).suffix();

        QMenu menu(this);
        QAction* run = menu.addAction(tr("运行"));
        QAction* runDebug = menu.addAction(tr("调试运行"));
        QAction* compile = menu.addAction(tr("编译…"));
        menu.addSeparator();
        QAction* close = menu.addAction(tr("关闭"));
        QAction* copy = menu.addAction(tr("复制路径"));
        const bool canRun = !readOnly && (suffix == "ax" || suffix == "axc" || suffix == "axp");
        run->setEnabled(canRun);
        runDebug->setEnabled(canRun);
        compile->setEnabled(!readOnly && (suffix == "ax" || suffix == "axc"));
        copy->setEnabled(!path.isEmpty());

        QAction* picked = menu.exec(m_list->viewport()->mapToGlobal(_pos));
        if (!picked) return;
        if (picked == run) emit runRequested(path);
        else if (picked == runDebug) emit runDebugRequested(path);
        else if (picked == compile) emit compileRequested(path);
        else if (picked == close) emit closeRequested(index);
        else if (picked == copy) QGuiApplication::clipboard()->setText(path);
    });
}

void OpenFilesPanel::setDocuments(const QList<EditorArea::OpenDoc>& _docs) {
    const QIcon lock = gui::icons::load(QStringLiteral("lock"));
    m_header->setText(tr("打开的文件") +
                      (_docs.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(_docs.size())));
    m_list->setUpdatesEnabled(false);
    m_list->clear();
    for (int i = 0; i < _docs.size(); ++i) {
        const auto& d = _docs[i];
        auto* item = new QListWidgetItem(d.title, m_list);
        item->setData(Qt::UserRole, i);
        item->setData(Qt::UserRole + 1, d.path);
        item->setData(Qt::UserRole + 2, d.readOnly);
        item->setToolTip(d.path.isEmpty() ? d.title : d.path);
        item->setIcon(d.readOnly ? lock
                                 : gui::icons::fileIcon(d.path.isEmpty()
                                                            ? QStringLiteral("axc") // untitled
                                                            : QFileInfo(d.path).suffix()));
        if (d.current) {
            QFont f = item->font();
            f.setBold(true);
            item->setFont(f);
            m_list->setCurrentItem(item);
        }
    }
    m_list->setUpdatesEnabled(true);
    if (!_docs.isEmpty() && m_list->currentRow() < 0) m_list->setCurrentRow(0);
}

int OpenFilesPanel::preferredHeight() const {
    const int rows = qBound(1, m_list->count(), 6);
    const int rowH = qMax(m_list->sizeHintForRow(0), fontMetrics().height() + 6);
    return m_header->sizeHint().height() + rows * rowH + 4;
}
