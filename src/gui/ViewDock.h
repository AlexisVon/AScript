#ifndef AGUI_VIEWDOCK_H
#define AGUI_VIEWDOCK_H

#include <QDockWidget>
#include <QVariantMap>

class QLabel;
class QPlainTextEdit;
class QStackedWidget;
class QTextBrowser;
class ImageCanvas;

// ViewDock — one dock for every anyptr view kind; the image page is page 0
class ViewDock : public QDockWidget {
    Q_OBJECT

public:
    explicit ViewDock(QWidget* _parent = nullptr);

    void clear_view();
    void show_failure(const QString& _ref, const QString& _type, const QString& _error);
    void show_view(const QString& _ref, const QString& _type, const QVariantMap& _view);

private:
    QString caption(const QString& _ref, const QString& _type, const QVariantMap& _view) const;
    void message_page(const QString& _text);

    QLabel* m_caption = nullptr;
    QStackedWidget* m_stack = nullptr;
    ImageCanvas* m_canvas = nullptr;
    QPlainTextEdit* m_text = nullptr;
    QTextBrowser* m_html = nullptr;
    QLabel* m_message = nullptr;
    QLabel* m_pixel = nullptr;
};

#endif // AGUI_VIEWDOCK_H
