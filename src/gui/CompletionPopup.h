#ifndef COMPLETIONPOPUP_H
#define COMPLETIONPOPUP_H

#include <QListWidget>

#include "CompletionItem.h"

// CompletionPopup — frameless suggestion list at the cursor (Qt::ToolTip keeps
// the editor focused and the IME unaffected). Keys are forwarded by
// ScriptEditor; click selects and inserts.
class CompletionPopup : public QListWidget {
    Q_OBJECT

public:
    explicit CompletionPopup(QWidget* _parent = nullptr);

    void showAt(const QPoint& _globalPos, const QList<CompletionItemData>& _items, int _windowRows);
    CompletionItemData currentData() const;

signals:
    void insertRequested(); // click / Enter / Tab — insertion is editor-side

protected:
    void mouseReleaseEvent(QMouseEvent* _ev) override;

private:
    class Delegate; // two-row painter: kind-colored name, dimmed sig + desc
};

#endif // COMPLETIONPOPUP_H
