#ifndef COMPLETIONITEM_H
#define COMPLETIONITEM_H

#include <QMetaType>
#include <QString>
#include <QStringList>

// CompletionItemData — one candidate row; the only contract shared by the base
// completion pool, the link/import hint provider and the popup renderer.
struct CompletionItemData {
    enum Kind { Extend,
                Define,
                Keyword,
                Builtin,
                Literal,
                Variable,
                Link,
                Import };
    QString name; // "$io_print" / "var" / "x"
    QString sig; // inline signature, dimmed: "(msg, ...)" / "= 3.14159"
    QString desc; // one-line description (second row)
    Kind kind = Extend;
    QStringList params; // placeholder param names — Enter inserts "name(p1, p2)" snippet
    QString source; // right-aligned provenance: "base" / "cv.so" / "utils.axc"
    bool call = true; // false for import variables — Enter inserts the bare name
};
Q_DECLARE_METATYPE(CompletionItemData)

#endif // COMPLETIONITEM_H
