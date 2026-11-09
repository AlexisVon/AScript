#ifndef QTCOMPAT_H
#define QTCOMPAT_H

// Qt version shims: the only place in src/gui that branches on QT_VERSION.

#include <QColor>
#include <QPalette>
#include <QtGlobal>

// QMouseEvent::globalPosition() (Qt6) / globalPos() (Qt5)
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#    define AXC_MOUSE_EVENT_GLOBAL_POS(_ev) ((_ev)->globalPosition().toPoint())
#else
#    define AXC_MOUSE_EVENT_GLOBAL_POS(_ev) ((_ev)->globalPos())
#endif

// QFontMetrics::horizontalAdvance() (5.11+) / width()
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
#    define AXC_FONT_METRICS_TEXT_WIDTH(_fm, _s) ((_fm).horizontalAdvance(_s))
#else
#    define AXC_FONT_METRICS_TEXT_WIDTH(_fm, _s) ((_fm).width(_s))
#endif

// QString::split separator behavior: Qt::SkipEmptyParts (5.14+) / QString::SkipEmptyParts
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
#    define AXC_STRING_SPLIT_SKIP_EMPTY_PARTS Qt::SkipEmptyParts
#else
#    define AXC_STRING_SPLIT_SKIP_EMPTY_PARTS QString::SkipEmptyParts
#endif

// QPlainTextEdit::setTabStopDistance() (5.10+) / setTabStopWidth()
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
#    define AXC_PLAIN_TEXT_EDIT_SET_TAB_STOP(_w, _px) ((_w)->setTabStopDistance(_px))
#else
#    define AXC_PLAIN_TEXT_EDIT_SET_TAB_STOP(_w, _px) ((_w)->setTabStopWidth(_px))
#endif

// QMenu::addAction() argument order: shortcut last before 6.4, shortcut second from 6.4
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
#    define AXC_MENU_ADD_ACTION_SHORTCUT(_menu, _text, _ctx, _slot, _shortcut) \
        ((_menu).addAction(_text, _shortcut, _ctx, _slot))
#else
#    define AXC_MENU_ADD_ACTION_SHORTCUT(_menu, _text, _ctx, _slot, _shortcut) \
        ((_menu).addAction(_text, _ctx, _slot, _shortcut))
#endif

// QKeySequenceEdit::setClearButtonEnabled() (6.4+); no clear button before that
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
#    define AXC_KEY_SEQUENCE_EDIT_SET_CLEAR_BUTTON(_w) ((_w)->setClearButtonEnabled(true))
#else
#    define AXC_KEY_SEQUENCE_EDIT_SET_CLEAR_BUTTON(_w) ((void) (_w))
#endif

// QGuiApplication::screenAt() (5.10+); linear search on older Qt
#include <QGuiApplication>
#include <QScreen>

inline QScreen* axc_screen_at(const QPoint& _p) {
    for (QScreen* s : QGuiApplication::screens())
        if (s->geometry().contains(_p)) return s;
    return QGuiApplication::primaryScreen();
}

#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
#    define AXC_GUI_APPLICATION_SCREEN_AT(_p) (QGuiApplication::screenAt(_p))
#else
#    define AXC_GUI_APPLICATION_SCREEN_AT(_p) (axc_screen_at(_p))
#endif

// QCoreApplication high-DPI attributes: opt-in on Qt5, always on in Qt6
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#    define AXC_CORE_APPLICATION_ENABLE_HIGHDPI()                        \
        do {                                                             \
            QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling); \
            QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);    \
        } while (0)
#else
#    define AXC_CORE_APPLICATION_ENABLE_HIGHDPI() ((void) 0)
#endif

// QPalette::PlaceholderText (5.12+); earlier versions use the text color at half opacity
inline QColor axc_placeholder_color(const QPalette& _p) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 12, 0)
    return _p.color(QPalette::PlaceholderText);
#else
    QColor c = _p.color(QPalette::Text);
    c.setAlphaF(0.5);
    return c;
#endif
}

#endif // QTCOMPAT_H
