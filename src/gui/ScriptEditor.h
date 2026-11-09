#ifndef SCRIPTEDITOR_H
#define SCRIPTEDITOR_H

#include <QPlainTextEdit>
#include <QSet>
#include <QSyntaxHighlighter>
#include <QTimer>

#include "CompletionPopup.h"

class AsHighlighter;
class HintProvider;
class QContextMenuEvent;
class QInputMethodEvent;
class QPaintEvent;
class QResizeEvent;
class QKeyEvent;
class QMouseEvent;

class ScriptEditor : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit ScriptEditor(QWidget* parent = nullptr);

    void setHighlighter(QSyntaxHighlighter* _hl); // takes ownership; replaces the default
    void setHintProvider(HintProvider* _provider); // link/import docs; owned by QScriptMain
    void setFilePath(const QString& _path); // module dir for hint resolution (empty = untitled)
    void applySettings(); // font/size + completion params from Settings (called on open + save)

    // line-number area: width + paint entry (canonical Qt pattern)
    int lineNumberAreaWidth() const;
    void lineNumberAreaPaintEvent(QPaintEvent* _e);

    // AST view mode: a read-only overlay shows the dump; the editor text itself
    // never changes, so no dirty bookkeeping is needed. restoreSource() hides it.
    void showAst(const QString& _ast);
    void restoreSource();
    bool isAstMode() const { return m_astMode; }

    // execution line highlight (1-based row); cleared on continue/finish
    void setExecutionLine(int _row);
    void clearExecutionLine();
    // breakpoint markers in the gutter; clicking a gutter row emits breakpointToggled
    void setBreakpoints(const QSet<int>& _rows);
    void gutterClicked(int _y);
    void gotoLine(int _row); // scroll the row into view (center)
    void rebuildSelections(); // current line + breakpoint hit (cursorPositionChanged fires often)

signals:
    void runRequested(); // "运行" in the context menu
    void astRequested(); // "查看 AST" / "返回原文" in the context menu
    void formatRequested(); // "格式化" — engine prtfmt on the doc text
    void breakpointToggled(int _row); // gutter click, 1-based row

protected:
    void resizeEvent(QResizeEvent* _e) override;
    void contextMenuEvent(QContextMenuEvent* _ev) override;
    void keyPressEvent(QKeyEvent* _ev) override; // forwards Up/Down/Enter/Esc to the popup
    void mousePressEvent(QMouseEvent* _ev) override; // any click closes the popup
    void mouseMoveEvent(QMouseEvent* _ev) override; // hover tooltip
    void leaveEvent(QEvent* _ev) override; // hide hover tooltip
    void inputMethodEvent(QInputMethodEvent* _ev) override; // track IME pre-edit state

private:
    void execContextMenu(const QPoint& _globalPos); // shared by editor + AST overlay
    void updateGutter(); // show/hide + size the line number gutter (hidden in AST mode)
    void layoutAstOverlay(); // AST banner + dump geometry

    // completion — auto popup on typing (debounce + params from Settings)
    void maybeShowCompletion();
    void hideCompletion();
    void insertCompletion(bool _asCall); // Tab → name; Enter → "name(p1, p2)" snippet
    void buildStaticItems(); // extend + define from core::document() + keyword tables

    // hover tooltip — show info for the word under the mouse
    void hideHoverTooltip();
    QList<CompletionItemData> collectMatches(const QString& _word) const;

    // line ops (VSCode semantics, shortcut table-driven)
    void cutLine(); // no selection → whole line to clipboard + delete
    void copyLine(); // no selection → whole line to clipboard
    void pasteLine(); // line clipboard → insert above the current line
    void moveLines(int _dir); // ±1: move the block group past the adjacent line

    QWidget* lineNumberArea;
    QSyntaxHighlighter* m_highlighter; // owned; default AsHighlighter, swappable
    QPlainTextEdit* m_astView; // read-only overlay in AST mode
    QWidget* m_astBar = nullptr; // "AST 视图 / 返回原文" banner above the dump
    CompletionPopup* m_popup = nullptr;
    QTimer m_completeTimer; // 300ms single-shot debounce
    QList<CompletionItemData> m_staticItems; // extend/define/keyword/builtin/literal candidates
    HintProvider* m_hints = nullptr;
    QString m_filePath; // current document path; empty = untitled
    // user-level candidates (user defs, variables, custom word lists) are a
    // reserved extension point — not implemented yet, see CompletionItemData::Variable
    bool m_suppress = false; // Esc — skip the next popup
    bool m_imeComposing = false; // true while the input method pre-edits (pinyin)
    QList<QPair<int, int>> m_snippetPos; // call-snippet placeholder (pos, len) — Tab walks them
    int m_snippetIdx = -1; // current placeholder, -1 = no active snippet
    int m_snippetBegin = 0; // inserted call span, kept current by contentsChange
    int m_snippetEnd = 0;
    bool m_astMode = false;
    bool m_showLineNumbers = true; // AST lines don't map to source rows — hide the gutter
    int m_execRow = -1; // paused execution row, -1 = none
    QSet<int> m_breakpoints; // rows with a marker

    // hover tooltip state
    QString m_hoveredWord; // last word the mouse rested on
    QTimer m_hoverTimer; // debounce before showing the tooltip
};
#endif // SCRIPTEDITOR_H
