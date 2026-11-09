#include "ViewDock.h"

#include <QColor>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QResizeEvent>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTextDocument>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>

// ImageCanvas — fit-to-window image page; wheel zooms, drag pans, double click refits.
// The crosshair follows the cursor and reports the image pixel under it.
class ImageCanvas : public QGraphicsView {
public:
    explicit ImageCanvas(QWidget* _parent = nullptr)
        : QGraphicsView(_parent) {
        m_scene = new QGraphicsScene(this);
        setScene(m_scene);
        m_item = m_scene->addPixmap(QPixmap());
        setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
        setBackgroundBrush(QColor(30, 30, 30));
        setTransformationAnchor(AnchorUnderMouse);
        setResizeAnchor(AnchorViewCenter);
        setDragMode(ScrollHandDrag);
        setMouseTracking(true);
    }

    void set_image(const QImage& _image) {
        m_image = _image;
        m_item->setPixmap(QPixmap::fromImage(_image));
        m_scene->setSceneRect(m_item->boundingRect());
        m_fitted = true;
        hide_cross();
        fit();
    }

    void clear_image() {
        m_image = QImage();
        m_item->setPixmap(QPixmap());
        m_scene->setSceneRect(QRectF());
        m_fitted = false;
        hide_cross();
    }

    // on_cursor — (x, y, color, gray); x < 0 when the cursor left the image
    std::function<void(int, int, QColor, bool)> on_cursor;

protected:
    void wheelEvent(QWheelEvent* _ev) override {
        if (m_item->pixmap().isNull() || _ev->angleDelta().y() == 0) return;
        const qreal factor = _ev->angleDelta().y() > 0 ? 1.25 : 0.8;
        const qreal next = transform().m11() * factor;
        if (next < 0.02 || next > 64) return;
        scale(factor, factor);
        m_fitted = false;
        _ev->accept();
    }

    void mouseDoubleClickEvent(QMouseEvent* _ev) override {
        (void) _ev;
        m_fitted = true;
        fit();
    }

    void mouseMoveEvent(QMouseEvent* _ev) override {
        QGraphicsView::mouseMoveEvent(_ev);
        if (m_image.isNull()) return;
        const QPointF scene = mapToScene(_ev->position().toPoint());
        const QRectF bounds = m_item->boundingRect();
        if (!bounds.contains(scene)) {
            hide_cross();
            return;
        }
        const int x = std::clamp(static_cast<int>(std::floor(scene.x())), 0, m_image.width() - 1);
        const int y = std::clamp(static_cast<int>(std::floor(scene.y())), 0, m_image.height() - 1);
        m_cursor = QPointF(x + 0.5, y + 0.5);
        m_cross = true;
        viewport()->update();
        if (on_cursor) on_cursor(x, y, m_image.pixelColor(x, y), m_image.isGrayscale());
    }

    void leaveEvent(QEvent* _ev) override {
        QGraphicsView::leaveEvent(_ev);
        hide_cross();
    }

    void resizeEvent(QResizeEvent* _ev) override {
        QGraphicsView::resizeEvent(_ev);
        if (m_fitted && !m_item->pixmap().isNull()) fit();
    }

    void drawForeground(QPainter* _p, const QRectF& _rect) override {
        (void) _rect;
        if (!m_cross) return;
        const QRectF bounds = m_item->boundingRect();
        QPen pen(QColor(255, 64, 64, 210));
        pen.setWidth(1);
        pen.setCosmetic(true);
        _p->save();
        _p->setRenderHint(QPainter::Antialiasing, false);
        _p->setPen(pen);
        _p->drawLine(QLineF(bounds.left(), m_cursor.y(), bounds.right(), m_cursor.y()));
        _p->drawLine(QLineF(m_cursor.x(), bounds.top(), m_cursor.x(), bounds.bottom()));
        _p->restore();
    }

private:
    void fit() {
        if (!m_item->pixmap().isNull()) fitInView(m_item, Qt::KeepAspectRatio);
    }

    void hide_cross() {
        if (!m_cross) return;
        m_cross = false;
        viewport()->update();
        if (on_cursor) on_cursor(-1, -1, QColor(), false);
    }

    QGraphicsScene* m_scene = nullptr;
    QGraphicsPixmapItem* m_item = nullptr;
    QImage m_image;
    QPointF m_cursor;
    bool m_fitted = false;
    bool m_cross = false;
};

ViewDock::ViewDock(QWidget* _parent)
    : QDockWidget(_parent) {
    setObjectName(QStringLiteral("dockView"));
    setWindowTitle(tr("视图"));
    setMinimumSize(260, 180);

    auto* body = new QWidget(this);
    auto* lay = new QVBoxLayout(body);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(4);

    m_caption = new QLabel(body);
    m_caption->setWordWrap(true);
    m_caption->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_stack = new QStackedWidget(body);
    m_canvas = new ImageCanvas(m_stack);
    m_text = new QPlainTextEdit(m_stack);
    m_text->setReadOnly(true);
    m_text->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_html = new QTextBrowser(m_stack);
    m_html->setOpenLinks(false);
    m_html->setOpenExternalLinks(false);
    m_html->setLineWrapMode(QTextEdit::WidgetWidth);
    m_html->document()->setDocumentMargin(8);
    m_message = new QLabel(m_stack);
    m_message->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_message->setWordWrap(true);
    m_message->setMargin(8);
    m_message->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_stack->addWidget(m_canvas);
    m_stack->addWidget(m_text);
    m_stack->addWidget(m_html);
    m_stack->addWidget(m_message);

    m_pixel = new QLabel(body);
    m_pixel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    lay->addWidget(m_caption);
    lay->addWidget(m_stack, 1);
    lay->addWidget(m_pixel);
    setWidget(body);

    m_canvas->on_cursor = [this](int _x, int _y, QColor _color, bool _gray) {
        if (_x < 0) {
            m_pixel->clear();
            return;
        }
        if (_gray)
            m_pixel->setText(tr("x=%1 y=%2  gray=%3").arg(_x).arg(_y).arg(_color.red()));
        else
            m_pixel->setText(tr("x=%1 y=%2  RGB(%3, %4, %5)  %6")
                                 .arg(_x)
                                 .arg(_y)
                                 .arg(_color.red())
                                 .arg(_color.green())
                                 .arg(_color.blue())
                                 .arg(_color.name(QColor::HexRgb).toUpper()));
    };
    clear_view();
}

void ViewDock::clear_view() {
    m_caption->clear();
    m_canvas->clear_image();
    m_text->clear();
    m_html->clear();
    m_pixel->clear();
    message_page(tr("未打开视图"));
}

void ViewDock::show_failure(const QString& _ref, const QString& _type, const QString& _error) {
    m_caption->setText(caption(_ref, _type, QVariantMap()));
    m_canvas->clear_image();
    m_text->clear();
    m_html->clear();
    m_pixel->clear();
    message_page(tr("打开失败:\n%1").arg(_error));
}

void ViewDock::show_view(const QString& _ref, const QString& _type, const QVariantMap& _view) {
    m_caption->setText(caption(_ref, _type, _view));
    const QString kind = _view.value(QStringLiteral("kind")).toString();
    if (kind == QLatin1String("text")) {
        m_canvas->clear_image();
        m_pixel->clear();
        m_text->setPlainText(QString::fromUtf8(_view.value(QStringLiteral("data")).toByteArray()));
        m_stack->setCurrentWidget(m_text);
        return;
    }
    if (kind == QLatin1String("html")) {
        m_canvas->clear_image();
        m_pixel->clear();
        m_html->setHtml(QString::fromUtf8(_view.value(QStringLiteral("data")).toByteArray()));
        m_stack->setCurrentWidget(m_html);
        return;
    }
    if (kind != QLatin1String("image")) {
        m_canvas->clear_image();
        m_text->clear();
        m_html->clear();
        m_pixel->clear();
        message_page(tr("不支持的视图类型: %1").arg(kind.isEmpty() ? QStringLiteral("?") : kind));
        return;
    }
    const QString format = _view.value(QStringLiteral("format")).toString();
    const QByteArray data = _view.value(QStringLiteral("data")).toByteArray();
    QImage image;
    if (!data.isEmpty()) {
        const QByteArray fmt = format.compare(QLatin1String("bmp"), Qt::CaseInsensitive) == 0
                                   ? QByteArrayLiteral("BMP")
                                   : QByteArrayLiteral("PNG");
        image.loadFromData(data, fmt.constData());
    }
    if (image.isNull()) {
        m_canvas->clear_image();
        m_text->clear();
        m_html->clear();
        m_pixel->clear();
        message_page(tr("图像解码失败（格式: %1，%2 字节）")
                         .arg(format.isEmpty() ? QStringLiteral("?") : format)
                         .arg(data.size()));
        return;
    }
    m_canvas->set_image(image);
    m_pixel->clear();
    m_stack->setCurrentWidget(m_canvas);
}

QString ViewDock::caption(const QString& _ref, const QString& _type, const QVariantMap& _view) const {
    QString text = _type.isEmpty() ? QStringLiteral("handle") : _type;
    if (!_ref.isEmpty()) text += QStringLiteral("  ") + _ref;
    if (_view.isEmpty()) return text;
    const qlonglong srcW = _view.value(QStringLiteral("srcWidth")).toLongLong();
    const qlonglong srcH = _view.value(QStringLiteral("srcHeight")).toLongLong();
    if (srcW > 0 && srcH > 0) text += QStringLiteral("  %1x%2").arg(srcW).arg(srcH);
    if (_view.value(QStringLiteral("normalized")).toBool()) text += tr("  (显示已归一化)");
    if (_view.value(QStringLiteral("scaled")).toBool()) text += tr("  (已缩放)");
    return text;
}

void ViewDock::message_page(const QString& _text) {
    m_message->setText(_text);
    m_stack->setCurrentWidget(m_message);
}
