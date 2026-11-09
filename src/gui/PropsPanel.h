#ifndef PROPSPANEL_H
#define PROPSPANEL_H

#include <QWidget>

class EmbedWidget;

// PropsPanel — tabbed file property panel (EmbedWidget popup).
// Page 1 "常规": path/type/size/modified/permissions.
// Page 2 ".axp 元数据": engine metadata table (only for .axp files).
class PropsPanel : public QWidget {
    Q_OBJECT

public:
    // show — create and display a property panel for _path (file or directory).
    // The panel is owned by _parent; hide = deleteLater.
    static void show(const QString& _path, QWidget* _parent);

private:
    explicit PropsPanel(QWidget* _parent = nullptr);
};

#endif // PROPSPANEL_H
