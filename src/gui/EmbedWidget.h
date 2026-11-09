#ifndef EMBEDWIDGET_H
#define EMBEDWIDGET_H

#include <QWidget>

#include <functional>

class QHBoxLayout;

namespace gui {
    QHBoxLayout* titleRow(const QString& _title, QWidget* _parent,
                          const std::function<void()>& _onClose);
}

// EmbedWidget — frameless child panel embedded in a parent window: draggable,
// clamped to the parent rect, with auto-position presets (center/corners).
// Used for popup panels like the file property viewer.
class EmbedWidget : public QWidget {
    Q_OBJECT

public:
    explicit EmbedWidget(QWidget* _parent = nullptr);
    ~EmbedWidget() override;

    // note: QWidget::show/hide/move are NOT virtual in Qt6 — these hide the
    // base-class names (same pattern as the original RegularParse widget)
    void show();
    void hide();
    void move(int _x, int _y);
    inline void move(const QSize _pos) { move(_pos.width(), _pos.height()); }
    inline void move(const QPoint _pos) { move(_pos.x(), _pos.y()); }

    inline bool is_auto_rect() const { return b_auto_rect; }
    inline void set_auto_rect(bool _enable) { b_auto_rect = _enable; }
    inline const QRect& get_rect() const { return ui_view_rect; }
    inline void set_rect(const QRect& _rect, bool _update) {
        ui_view_rect = _rect;
        if (_update) move(pos());
    }

    void move_center();
    void move_top_center(int _y = 0);
    void move_right_top();
    void move_right_bottom();
    void move_left_top();
    void move_left_bottom();

signals:
    void sig_visible(bool _visible);

protected:
    void resizeEvent(QResizeEvent* _ev) override;
    void mouseMoveEvent(QMouseEvent* _ev) override;
    void mousePressEvent(QMouseEvent* _ev) override;
    void mouseReleaseEvent(QMouseEvent* _ev) override;

private:
    QRect ui_view_rect;
    QPoint ui_mouse_point;
    bool ui_mouse_press{false};
    bool b_auto_rect{true};
};
#endif // EMBEDWIDGET_H
