#ifndef ASC_ICONS_H
#define ASC_ICONS_H

#include <QColor>
#include <QHash>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QString>
#include <QSvgRenderer>

namespace gui::icons {

    // load — SVG at every UI size (16–64px); optional color tints the strokes
    inline QIcon load(const QString& _name, const QColor& _color = QColor()) {
        const QString key = _name + (_color.isValid() ? _color.name(QColor::HexArgb) : QString());
        static auto* cache = new QHash<QString, QIcon>; // leaked: QIcon must not outlive QApplication
        if (const auto it = cache->constFind(key); it != cache->constEnd()) return it.value();

        QSvgRenderer r(QStringLiteral(":/ui/icons/%1.svg").arg(_name));
        Q_ASSERT_X(r.isValid(), "icons::load", qPrintable(_name));
        QIcon icon;
        for (int size : {16, 20, 24, 28, 32, 40, 48, 64}) {
            QPixmap pm(size, size);
            pm.fill(Qt::transparent);
            QPainter p(&pm);
            r.render(&p);
            if (_color.isValid()) {
                p.setCompositionMode(QPainter::CompositionMode_SourceIn);
                p.fillRect(pm.rect(), _color);
            }
            p.end();
            icon.addPixmap(pm);
        }
        cache->insert(key, icon);
        return icon;
    }

    // fileColor — gruvbox accent per source/data extension; invalid = default text
    inline QColor fileColor(const QString& _suffix) {
        const QString s = _suffix.toLower();
        if (s == QLatin1String("ax")) return QColor(0xb8, 0xbb, 0x26);
        if (s == QLatin1String("axc")) return QColor(0x83, 0xa5, 0x98);
        if (s == QLatin1String("axp")) return QColor(0xd3, 0x86, 0x9b);
        if (s == QLatin1String("json")) return QColor(0xfa, 0xbd, 0x2f);
        if (s == QLatin1String("md")) return QColor(0x8e, 0xc0, 0x7c);
        return QColor();
    }

    // fileIcon — document glyph tinted by file type (default grey)
    inline QIcon fileIcon(const QString& _suffix) {
        const QColor c = fileColor(_suffix);
        return load(QStringLiteral("file"), c.isValid() ? c : QColor(0xa8, 0x99, 0x84));
    }

} // namespace gui::icons

#endif // ASC_ICONS_H
