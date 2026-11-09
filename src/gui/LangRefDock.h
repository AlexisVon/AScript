#ifndef LANGREFDOCK_H
#define LANGREFDOCK_H

#include <QHash>
#include <QList>
#include <QMap>
#include <QWidget>

class QLineEdit;
class QShowEvent;
class HintProvider;
class QTextBrowser;
class QTreeWidget;
class QTreeWidgetItem;

namespace alx {
    class json_array;
}

// LangRefDock — split language reference: chapter tree + rendered blocks, plus
// a function manual generated from core::document() and a name/description filter.
class LangRefDock : public QWidget {
    Q_OBJECT

public:
    explicit LangRefDock(QWidget* parent = nullptr);
    void applySettings(); // reading font from the editor settings
    void setHintProvider(HintProvider* _hints);
    void refreshLibraries(); // rebuild the "库" root from the shared hint roots

protected:
    void showEvent(QShowEvent* _ev) override;

private:
    struct Section {
        int block; // index into core::langref() blocks
        int level; // 2 or 3
        QString title;
    };
    struct Chapter {
        int begin = 0; // first block (the level-1 heading)
        int end = 0; // one past the last block
        QString title;
        QList<Section> sections;
    };
    void build(); // index core::langref() into chapters + generate the function manual, once
    void buildFunctionTree(QTreeWidgetItem* _root);
    void renderChapter(int _index);
    void scrollToBlock(int _block);
    void renderFunction(const QString& _key);
    void renderModule(const QString& _module);
    void renderLibrary(int _kind, const QString& _path);
    void filterTree(const QString& _text);
    QTreeWidgetItem* firstVisibleFunction(QTreeWidgetItem* _item) const;
    void activateItem(QTreeWidgetItem* _item);

    QLineEdit* m_search;
    QTreeWidget* m_tree;
    QTextBrowser* m_view;
    HintProvider* m_hints = nullptr;
    QTreeWidgetItem* m_libRoot = nullptr;
    const alx::json_array* m_blocks = nullptr;
    QList<Chapter> m_chapters;
    QMap<QString, QStringList> m_modules; // module -> bare keys, functions then defines
    QHash<QString, QString> m_funcHtml; // bare key -> detail page
    QHash<QString, QString> m_funcText; // bare key -> lowercase search text
    QHash<QString, QString> m_funcSig; // bare key -> first-overload signature
    QHash<QString, QString> m_funcDesc; // bare key -> first-overload description
    bool m_built = false;
};

#endif // LANGREFDOCK_H
