#include "WelcomePage.h"

#include "Icons.h"
#include "Settings.h"

#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QShowEvent>
#include <QVBoxLayout>

WelcomePage::WelcomePage(QWidget* parent)
    : QWidget(parent) {
    auto* inner = new QWidget(this);
    inner->setMaximumWidth(560);

    auto* title = new QLabel(QStringLiteral("AScript"), inner);
    QFont titleFont = title->font();
    titleFont.setPointSize(28);
    titleFont.setBold(true);
    title->setFont(titleFont);
    title->setStyleSheet(QStringLiteral("color:#fbf1c7;background:transparent;"));
    auto* subtitle = new QLabel(tr("脚本编辑器 / 调试器"), inner);
    subtitle->setStyleSheet(QStringLiteral("color:#928374;background:transparent;"));

    auto makeButton = [this, inner](const QString& _text, const QString& _icon) {
        auto* b = new QPushButton(gui::icons::load(_icon), _text, inner);
        b->setFixedHeight(32);
        b->setCursor(Qt::PointingHandCursor);
        return b;
    };
    auto* newBtn = makeButton(tr("新建脚本"), QStringLiteral("file-plus"));
    auto* openBtn = makeButton(tr("打开文件"), QStringLiteral("file"));
    auto* dirBtn = makeButton(tr("打开目录"), QStringLiteral("folder-open"));
    connect(newBtn, &QPushButton::clicked, this, &WelcomePage::newRequested);
    connect(openBtn, &QPushButton::clicked, this, &WelcomePage::openFileRequested);
    connect(dirBtn, &QPushButton::clicked, this, &WelcomePage::openDirRequested);

    auto* buttonRow = new QHBoxLayout;
    buttonRow->setSpacing(8);
    buttonRow->addWidget(newBtn);
    buttonRow->addWidget(openBtn);
    buttonRow->addWidget(dirBtn);
    buttonRow->addStretch();

    auto* recentTitle = new QLabel(tr("最近打开"), inner);
    recentTitle->setStyleSheet(QStringLiteral("color:#d5c4a1;background:transparent;"));
    m_recent = new QListWidget(inner);
    m_recent->setFrameShape(QFrame::NoFrame);
    m_recent->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_recent->setSelectionMode(QAbstractItemView::SingleSelection);
    auto openItem = [this](QListWidgetItem* _item) {
        if (_item) emit recentRequested(_item->data(Qt::UserRole).toString());
    };
    connect(m_recent, &QListWidget::itemClicked, this, openItem);

    auto* hint = new QLabel(
        tr("F5 调试运行 · Ctrl+F5 运行 · Ctrl+S 保存 · Alt+Shift+F 格式化"), inner);
    hint->setStyleSheet(QStringLiteral("color:#665c54;background:transparent;"));

    auto* innerLay = new QVBoxLayout(inner);
    innerLay->setContentsMargins(0, 0, 0, 0);
    innerLay->setSpacing(8);
    innerLay->addWidget(title);
    innerLay->addWidget(subtitle);
    innerLay->addSpacing(16);
    innerLay->addLayout(buttonRow);
    innerLay->addSpacing(20);
    innerLay->addWidget(recentTitle);
    innerLay->addWidget(m_recent);
    innerLay->addSpacing(8);
    innerLay->addWidget(hint);

    auto* center = new QHBoxLayout;
    center->addStretch(1);
    center->addWidget(inner);
    center->addStretch(1);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(48, 32, 48, 32);
    lay->addStretch(1);
    lay->addLayout(center);
    lay->addStretch(1);

    refreshRecent();
}

void WelcomePage::refreshRecent() {
    m_recent->clear();
    const QStringList paths = Settings::recentPaths();
    if (paths.isEmpty()) {
        auto* item = new QListWidgetItem(tr("暂无最近打开"), m_recent);
        item->setFlags(Qt::NoItemFlags);
        item->setForeground(QColor(0x92, 0x83, 0x74));
        m_recent->setFixedHeight(34);
        return;
    }
    for (const QString& path : paths) {
        const QFileInfo fi(path);
        const QString name = fi.isDir() ? fi.fileName() + "/" : fi.fileName();
        auto* item = new QListWidgetItem(m_recent); // item widget draws both lines
        item->setData(Qt::AccessibleTextRole, name);
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
        item->setSizeHint(QSize(0, 46));

        auto* row = new QWidget;
        row->setAttribute(Qt::WA_TransparentForMouseEvents);
        row->setStyleSheet(QStringLiteral("background:transparent;"));
        auto* lay = new QVBoxLayout(row);
        lay->setContentsMargins(6, 3, 6, 3);
        lay->setSpacing(0);
        auto* nameLabel = new QLabel(name, row);
        nameLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        nameLabel->setStyleSheet(QStringLiteral("color:#ebdbb2;background:transparent;"));
        auto* pathLabel = new QLabel(QDir::toNativeSeparators(path), row);
        pathLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        pathLabel->setStyleSheet(QStringLiteral("color:#928374;background:transparent;"));
        QFont pathFont = pathLabel->font();
        pathFont.setPointSize(qMax(7, pathFont.pointSize() - 2));
        pathLabel->setFont(pathFont);
        lay->addWidget(nameLabel);
        lay->addWidget(pathLabel);
        m_recent->setItemWidget(item, row);
    }
    m_recent->setFixedHeight(qMin(paths.size(), 6) * 46 + 8);
    m_recent->setCurrentRow(-1);
}

void WelcomePage::showEvent(QShowEvent* _ev) {
    QWidget::showEvent(_ev);
    refreshRecent();
}
