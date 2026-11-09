#include "EmbedWidget.h"

#include "Icons.h"
#include "QtCompat.h"

#include <QColor>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QToolButton>

QHBoxLayout* gui::titleRow(const QString& _title, QWidget* _parent,
                           const std::function<void()>& _onClose) {
    auto* row = new QHBoxLayout;
    auto* title = new QLabel(_title, _parent);
    QFont tf = title->font();
    tf.setBold(true);
    title->setFont(tf);
    auto* closeBtn = new QToolButton(_parent);
    closeBtn->setFixedSize(24, 24);
    closeBtn->setIcon(gui::icons::load(QStringLiteral("close")));
    closeBtn->setIconSize(QSize(14, 14));
    closeBtn->setAutoRaise(true);
    closeBtn->setToolTip(QObject::tr("关闭"));
    QObject::connect(closeBtn, &QToolButton::clicked, _parent, _onClose);
    row->addWidget(title);
    row->addStretch();
    row->addWidget(closeBtn);
    return row;
}

EmbedWidget::EmbedWidget(QWidget* _parent)
    : QWidget(_parent) {
    setWindowFlags(Qt::FramelessWindowHint);
    setAttribute(Qt::WA_StyledBackground, true); // draw QSS background/border
    auto* shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(28);
    shadow->setOffset(0, 8);
    shadow->setColor(QColor(0, 0, 0, 180));
    setGraphicsEffect(shadow);
    if (_parent != nullptr) set_rect(_parent->rect(), false);
}

EmbedWidget::~EmbedWidget() {}

void EmbedWidget::show() {
    QWidget::show();
    emit sig_visible(true);
}

void EmbedWidget::hide() {
    QWidget::hide();
    emit sig_visible(false);
}

void EmbedWidget::move(int _x, int _y) {
    if (ui_view_rect.isEmpty()) return QWidget::move(_x, _y);
    QWidget::move(qBound(ui_view_rect.left(), _x, ui_view_rect.right() - width()),
                  qBound(ui_view_rect.top(), _y, ui_view_rect.bottom() - height()));
}

void EmbedWidget::move_center() {
    // refresh the anchor rect — panels created during the parent's constructor
    // captured a stale rect (parent not laid out yet); recenter against the
    // parent's current rect every time
    if (parentWidget() != nullptr) set_rect(parentWidget()->rect(), false);
    move(ui_view_rect.center() - rect().center());
}

void EmbedWidget::move_top_center(int _y) {
    if (parentWidget() != nullptr) set_rect(parentWidget()->rect(), false);
    move(ui_view_rect.center().x() - width() / 2, ui_view_rect.top() + _y);
}

void EmbedWidget::move_right_top() {
    move(ui_view_rect.topRight());
}

void EmbedWidget::move_right_bottom() {
    move(ui_view_rect.bottomRight());
}

void EmbedWidget::move_left_top() {
    move(ui_view_rect.topLeft());
}

void EmbedWidget::move_left_bottom() {
    move(ui_view_rect.bottomLeft());
}

void EmbedWidget::resizeEvent(QResizeEvent* _ev) {
    if (!b_auto_rect || parentWidget() == nullptr) return;
    set_rect(parentWidget()->rect(), true);
    QWidget::resizeEvent(_ev);
}

void EmbedWidget::mouseMoveEvent(QMouseEvent* _ev) {
    if (ui_mouse_press) move(AXC_MOUSE_EVENT_GLOBAL_POS(_ev) - ui_mouse_point);
}

void EmbedWidget::mousePressEvent(QMouseEvent* _ev) {
    if (_ev->button() == Qt::LeftButton && !windowState().testFlag(Qt::WindowFullScreen)) {
        ui_mouse_press = true;
        ui_mouse_point = AXC_MOUSE_EVENT_GLOBAL_POS(_ev) - this->pos();
    }
}

void EmbedWidget::mouseReleaseEvent(QMouseEvent* _ev) {
    ui_mouse_press = false;
}
