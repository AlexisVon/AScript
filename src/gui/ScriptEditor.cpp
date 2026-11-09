#include "ScriptEditor.h"

#include "AsHighlighter.h"
#include "HintProvider.h"
#include "QtCompat.h"
#include "Settings.h"
#include "core/core.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QContextMenuEvent>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QToolButton>
#include <QToolTip>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFormat>

#include <algorithm>

// LineNumberArea — gutter painted alongside the editor viewport
class LineNumberArea : public QWidget {
public:
    explicit LineNumberArea(ScriptEditor* _editor)
        : QWidget(_editor), editor(_editor) {}

    QSize sizeHint() const override {
        return {editor->lineNumberAreaWidth(), 0};
    }

protected:
    void paintEvent(QPaintEvent* _e) override {
        editor->lineNumberAreaPaintEvent(_e);
    }

    void mousePressEvent(QMouseEvent* _e) override {
        if (_e->button() == Qt::LeftButton) editor->gutterClicked(_e->pos().y());
    }

private:
    ScriptEditor* editor;
};

ScriptEditor::ScriptEditor(QWidget* parent)
    : QPlainTextEdit(parent),
      lineNumberArea(new LineNumberArea(this)),
      m_highlighter(new AsHighlighter(document())) {
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);

    // gruvbox dark editor palette, QSS supplies the chrome
    QPalette pal;
    pal.setColor(QPalette::Base, QColor(0x28, 0x28, 0x28)); // bg0
    pal.setColor(QPalette::Text, QColor(0xeb, 0xdb, 0xb2)); // fg1
    pal.setColor(QPalette::Highlight, QColor(0x50, 0x49, 0x45)); // bg2
    pal.setColor(QPalette::HighlightedText, QColor(0xfb, 0xf1, 0xc7)); // fg0
    setPalette(pal);

    // selections rebuilt on every cursor move: current line + breakpoint hit (if any)
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] { rebuildSelections(); });

    connect(this, &QPlainTextEdit::blockCountChanged, this, [this](int) {
        // digit count changed: margins AND gutter width must follow, else the
        // gutter falls behind and overlaps the text
        updateGutter();
    });
    connect(this, &QPlainTextEdit::updateRequest, this,
            [this](const QRect& _rect, int _dy) {
                if (_dy) lineNumberArea->scroll(0, _dy);
                else lineNumberArea->update(0, _rect.y(), lineNumberArea->width(), _rect.height());
            });
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        lineNumberArea->update();
    });

    // AST view overlay: read-only, covers the viewport (right of the gutter);
    // own context menu (copy/select-all/back) instead of the Qt default one
    m_astView = new QPlainTextEdit(this);
    m_astView->setReadOnly(true);
    m_astView->setPalette(pal);
    new AsAstHighlighter(m_astView->document()); // colored prtast dump
    // same context menu as the editor; in AST mode the edit entries are disabled
    m_astView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_astView, &QPlainTextEdit::customContextMenuRequested, this,
            [this](const QPoint& _pos) {
                execContextMenu(m_astView->viewport()->mapToGlobal(_pos));
            });
    m_astView->hide();

    // AST banner: a visible way back (Alt+A and the context menu still work)
    m_astBar = new QWidget(this);
    m_astBar->setObjectName(QStringLiteral("astBar"));
    auto* astLay = new QHBoxLayout(m_astBar);
    astLay->setContentsMargins(8, 0, 4, 0);
    astLay->setSpacing(6);
    auto* astLabel = new QLabel(tr("AST 视图"), m_astBar);
    astLabel->setObjectName(QStringLiteral("astBarLabel"));
    auto* astBack = new QToolButton(m_astBar);
    astBack->setObjectName(QStringLiteral("astBack"));
    astBack->setText(tr("返回原文"));
    astBack->setAutoRaise(true);
    astLay->addWidget(astLabel);
    astLay->addStretch();
    astLay->addWidget(astBack);
    connect(astBack, &QToolButton::clicked, this, &ScriptEditor::astRequested);
    m_astBar->hide();

    // completion — auto popup 300ms after typing pauses; IME pre-edit excluded
    // in maybeShowCompletion, Esc suppresses the popup for the next keystroke
    buildStaticItems();
    m_popup = new CompletionPopup(this);
    m_completeTimer.setSingleShot(true);
    m_completeTimer.setInterval(Settings::completionDelay());
    connect(&m_completeTimer, &QTimer::timeout, this, &ScriptEditor::maybeShowCompletion);
    connect(this, &QPlainTextEdit::textChanged, this, [this] {
        if (m_suppress) {
            m_suppress = false;
            return;
        }
        hideHoverTooltip();
        m_completeTimer.start();
    });
    connect(m_popup, &CompletionPopup::insertRequested, this, [this] { insertCompletion(false); }); // click → name
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        // leave the call-snippet once the cursor moves off the inserted call
        if (m_snippetIdx < 0) return;
        const int pos = textCursor().position();
        if (pos < m_snippetBegin || pos > m_snippetEnd) {
            m_snippetPos.clear();
            m_snippetIdx = -1;
            return;
        }
        for (int i = 0; i < m_snippetPos.size(); ++i)
            if (pos >= m_snippetPos[i].first && pos <= m_snippetPos[i].first + m_snippetPos[i].second) {
                m_snippetIdx = i;
                break;
            }
    });
    connect(document(), &QTextDocument::contentsChange, this, [this](int _pos, int _removed, int _added) {
        // keep the snippet's absolute coordinates current while the user edits
        if (m_snippetIdx < 0) return;
        const int delta = _added - _removed;
        if (delta == 0) return;
        if (_pos + _removed <= m_snippetBegin) {
            m_snippetBegin += delta;
            m_snippetEnd += delta;
        } else if (_pos < m_snippetEnd) {
            m_snippetEnd = qMax(m_snippetBegin, m_snippetEnd + delta);
        }
        for (auto& sp : m_snippetPos) {
            if (_pos + _removed <= sp.first) sp.first += delta;
            else if (_pos < sp.first + sp.second) sp.second = qMax(0, sp.second + delta);
        }
    });

    // hover tooltip — show info for the word under the mouse
    viewport()->setMouseTracking(true);
    m_hoverTimer.setSingleShot(true);
    m_hoverTimer.setInterval(150);
    connect(&m_hoverTimer, &QTimer::timeout, this, [this] {
        if (m_hoveredWord.isEmpty()) return;
        QTextCursor c = cursorForPosition(mapFromGlobal(QCursor::pos()));
        QString word;
        {
            const QString line = c.block().text();
            int p = c.positionInBlock();
            auto isId = [](QChar ch) { return ch.isLetterOrNumber() || ch == '_' || ch == '$'; };
            int s = p;
            while (s > 0 && isId(line[s - 1])) --s;
            int e = p;
            while (e < line.size() && isId(line[e])) ++e;
            word = line.mid(s, e - s);
        }
        if (word != m_hoveredWord || m_hoveredWord.isEmpty()) return;
        const auto matches = collectMatches(word);
        if (matches.isEmpty()) return;

        const QFont ef(Settings::editorFont(), Settings::hoverFontSize());
        const int sz = ef.pointSize() > 0 ? ef.pointSize() : 11;
        const QString ff = ef.family();
        const QString fs = QString::number(sz) + "pt";
        const QString fsDim = QString::number(qMax(sz - 1, 8)) + "pt";

        static const char* kindLabels[] = {"extend", "define", "keyword", "builtin", "literal", "variable", "link", "import"};
        struct KindGroup {
            const char* label;
            QList<const CompletionItemData*> items;
        };
        QList<KindGroup> groups;
        for (const auto& m : matches) {
            auto it = std::find_if(groups.begin(), groups.end(),
                                   [&](const KindGroup& g) { return g.label == kindLabels[m.kind]; });
            if (it == groups.end())
                groups.append({kindLabels[m.kind], {&m}});
            else
                it->items.append(&m);
        }

        QString html = QString("<style>"
                               "body{font-family:'%1';font-size:%2}"
                               ".name{font-weight:bold;color:#83a598}"
                               ".sig{font-family:'%1',monospace;color:#ebdbb2}"
                               ".dim{color:#a89984;font-size:%3}"
                               "</style><body>")
                           .arg(ff, fs, fsDim);
        for (const auto& g : groups) {
            html += QString("<div class='name'>%1</div>").arg(g.items.first()->name.toHtmlEscaped());
            for (const auto* item : g.items) {
                if (!item->sig.isEmpty())
                    html += QString("<div class='sig'>%1</div>").arg(item->sig.toHtmlEscaped());
                if (!item->desc.isEmpty())
                    html += QString("<div class='dim'>%1</div>").arg(item->desc.toHtmlEscaped());
            }
        }
        html += "</body>";
        QToolTip::setFont(ef);
        QToolTip::showText(QCursor::pos(), html, this);
    });

    applySettings(); // font + completion params (needs m_astView/m_completeTimer)
    lineNumberArea->update();
    lineNumberArea->setMouseTracking(true);
}

void ScriptEditor::setHighlighter(QSyntaxHighlighter* _hl) {
    delete m_highlighter;
    m_highlighter = _hl;
    m_highlighter->rehighlight();
}

void ScriptEditor::setHintProvider(HintProvider* _provider) {
    m_hints = _provider;
}

void ScriptEditor::setFilePath(const QString& _path) {
    m_filePath = _path;
}

void ScriptEditor::applySettings() {
    const QFont f(Settings::editorFont(), Settings::editorFontSize());
    setFont(f);
    m_astView->setFont(f);
    m_popup->setFont(QFont(Settings::editorFont(), Settings::completionFontSize()));
    AXC_PLAIN_TEXT_EDIT_SET_TAB_STOP(this, AXC_FONT_METRICS_TEXT_WIDTH(fontMetrics(), QLatin1Char(' ')) * 4);
    m_completeTimer.setInterval(Settings::completionDelay());
    updateGutter(); // font metrics changed — gutter width must follow
}

int ScriptEditor::lineNumberAreaWidth() const {
    int digits = 1;
    int max = std::max(1, blockCount());
    while (max >= 10) {
        max /= 10;
        ++digits;
    }
    // 12px marker column (breakpoint dot) + 6px right padding
    return 18 + AXC_FONT_METRICS_TEXT_WIDTH(fontMetrics(), QLatin1Char('9')) * digits;
}

void ScriptEditor::setBreakpoints(const QSet<int>& _rows) {
    if (m_breakpoints == _rows) return;
    m_breakpoints = _rows;
    if (lineNumberArea) lineNumberArea->update();
}

void ScriptEditor::gutterClicked(int _y) {
    const QTextCursor c = cursorForPosition(QPoint(0, _y));
    emit breakpointToggled(c.blockNumber() + 1);
}

void ScriptEditor::lineNumberAreaPaintEvent(QPaintEvent* _e) {
    QPainter painter(lineNumberArea);
    painter.fillRect(_e->rect(), QColor(0x28, 0x28, 0x28)); // bg0

    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = static_cast<int>(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + static_cast<int>(blockBoundingRect(block).height());

    while (block.isValid() && top <= _e->rect().bottom()) {
        if (block.isVisible() && bottom >= _e->rect().top()) {
            painter.setPen(QColor(0xa8, 0x99, 0x84)); // fg4
            painter.drawText(0, top, lineNumberArea->width() - 6, fontMetrics().height(),
                             Qt::AlignRight, QString::number(blockNumber + 1));
            if (m_breakpoints.contains(blockNumber + 1)) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(0xfb, 0x49, 0x34)); // gruvbox red
                const int dot = 8;
                painter.drawEllipse(QRect(3, top + (fontMetrics().height() - dot) / 2, dot, dot));
            }
        }
        block = block.next();
        top = bottom;
        bottom = top + static_cast<int>(blockBoundingRect(block).height());
        ++blockNumber;
    }
}

void ScriptEditor::resizeEvent(QResizeEvent* _e) {
    QPlainTextEdit::resizeEvent(_e);
    const QRect cr = contentsRect();
    lineNumberArea->setGeometry(QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
    layoutAstOverlay();
}

void ScriptEditor::contextMenuEvent(QContextMenuEvent* _ev) {
    execContextMenu(_ev->globalPos());
}

void ScriptEditor::execContextMenu(const QPoint& _globalPos) {
    QMenu menu(this);
    const bool ro = isAstMode(); // AST view: editing entries disabled
    AXC_MENU_ADD_ACTION_SHORTCUT(menu, "剪切", this, &QPlainTextEdit::cut, QKeySequence::Cut)->setEnabled(!ro);
    AXC_MENU_ADD_ACTION_SHORTCUT(menu, "复制", this, &QPlainTextEdit::copy, QKeySequence::Copy);
    AXC_MENU_ADD_ACTION_SHORTCUT(menu, "粘贴", this, &QPlainTextEdit::paste, QKeySequence::Paste)->setEnabled(!ro);
    menu.addSeparator();
    AXC_MENU_ADD_ACTION_SHORTCUT(menu, "撤销", this, &QPlainTextEdit::undo, QKeySequence::Undo)->setEnabled(!ro);
    AXC_MENU_ADD_ACTION_SHORTCUT(menu, "重做", this, &QPlainTextEdit::redo, QKeySequence::Redo)->setEnabled(!ro);
    menu.addSeparator();
    menu.addAction("运行", this, [this] { emit runRequested(); })->setEnabled(!ro);
    menu.addAction("格式化", this, [this] { emit formatRequested(); })->setEnabled(!ro);
    menu.addAction(isAstMode() ? "返回原文" : "查看 AST", this, [this] { emit astRequested(); });
    menu.exec(_globalPos);
}

void ScriptEditor::updateGutter() {
    // digit count changed: margins AND gutter width must follow, else the
    // gutter falls behind and overlaps the text; AST mode hides the gutter
    // (AST rows don't map to source lines)
    const int w = m_showLineNumbers ? lineNumberAreaWidth() : 0;
    setViewportMargins(w, 0, 0, 0);
    if (w == 0) {
        lineNumberArea->hide();
        return;
    }
    lineNumberArea->show();
    lineNumberArea->setGeometry(QRect(contentsRect().left(), contentsRect().top(),
                                      w, contentsRect().height()));
    lineNumberArea->update();
}

void ScriptEditor::layoutAstOverlay() {
    if (!m_astBar) return; // resize can arrive before the banner exists
    const QRect vp = viewport()->geometry();
    const int barH = m_astMode ? 26 : 0;
    m_astBar->setGeometry(vp.left(), vp.top(), vp.width(), barH);
    m_astView->setGeometry(vp.left(), vp.top() + barH, vp.width(), vp.height() - barH);
}

void ScriptEditor::showAst(const QString& _ast) {
    m_astView->setPlainText(_ast);
    m_astMode = true;
    m_showLineNumbers = false;
    updateGutter();
    setReadOnly(true); // the underlying editor must not accept keys either
    layoutAstOverlay();
    m_astBar->show();
    m_astView->show();
    m_astView->raise();
    m_astBar->raise();
    m_astView->setFocus(); // keys land on the read-only overlay
    m_astView->moveCursor(QTextCursor::Start);
}

void ScriptEditor::restoreSource() {
    m_astMode = false;
    m_showLineNumbers = true;
    updateGutter();
    m_astBar->hide();
    m_astView->hide();
    layoutAstOverlay();
    setReadOnly(false);
    setFocus();
}

void ScriptEditor::setExecutionLine(int _row) {
    m_execRow = _row;
    rebuildSelections();
}

void ScriptEditor::clearExecutionLine() {
    if (m_execRow < 0) return;
    m_execRow = -1;
    rebuildSelections();
}

void ScriptEditor::rebuildSelections() {
    QList<QTextEdit::ExtraSelection> sel;
    QTextEdit::ExtraSelection cur; // current line (vim-style), bg1 — painted first (bottom)
    cur.format.setBackground(QColor(0x3c, 0x38, 0x36));
    cur.format.setProperty(QTextFormat::FullWidthSelection, true);
    cur.cursor = textCursor();
    cur.cursor.clearSelection();
    sel.append(cur);
    if (m_execRow >= 0) { // paused statement, gruvbox blue — last = topmost, covers the cursor line
        QTextEdit::ExtraSelection hit;
        hit.format.setBackground(QColor(0x83, 0xa5, 0x98, 100)); // muted teal over the line bg
        hit.format.setForeground(QColor(0xfb, 0xf1, 0xc7));
        hit.format.setProperty(QTextFormat::FullWidthSelection, true);
        hit.cursor = QTextCursor(document()->findBlockByNumber(m_execRow - 1));
        hit.cursor.clearSelection();
        sel.append(hit);
    }
    setExtraSelections(sel);
}

void ScriptEditor::gotoLine(int _row) {
    QTextCursor c(document()->findBlockByNumber(_row - 1));
    setTextCursor(c);
    centerCursor();
}

void ScriptEditor::maybeShowCompletion() {
    if (m_astMode || isReadOnly()) return;
    if (!Settings::completionEnabled()) {
        hideCompletion();
        return;
    }
    if (m_imeComposing) return; // pinyin pre-edit — no popup

    const QTextCursor c = textCursor();
    const QString line = c.block().text();
    const int pos = c.positionInBlock();
    auto isId = [](QChar _ch) { return _ch.isLetterOrNumber() || _ch == '_' || _ch == '$'; };
    int start = pos;
    while (start > 0 && isId(line[start - 1])) --start;

    const QString pre = line.left(start);
    if (pre.count('"') % 2 || pre.count('\'') % 2 || pre.count('`') % 2 ||
        pre.contains("//") || pre.lastIndexOf("/*") > pre.lastIndexOf("*/")) {
        hideCompletion(); // inside a string or comment (per-line approximation)
        return;
    }

    // dot context — the head segment must be a declared link/import alias
    if (start > 0 && line[start - 1] == '.') {
        int chain = start - 1;
        while (chain > 0 && (isId(line[chain - 1]) || line[chain - 1] == '.')) --chain;
        const QString chainText = line.mid(chain, start - 1 - chain);
        if (!chainText.isEmpty() && m_hints) {
            QList<CompletionItemData> cands = m_hints->complete(
                toPlainText(), m_filePath, chainText, line.mid(start, pos - start));
            if (!cands.isEmpty()) {
                if (!Settings::completionShowDesc())
                    for (auto& d : cands) d.desc.clear();
                const int listLimit = Settings::completionListLimit(); // 0 = unlimited
                if (listLimit > 0 && cands.size() > listLimit) cands = cands.mid(0, listLimit);
                hideHoverTooltip();
                m_popup->showAt(viewport()->mapToGlobal(cursorRect().bottomLeft()) + QPoint(0, 4), cands,
                                Settings::completionWindowItems());
                return;
            }
        }
        hideCompletion();
        return;
    }

    if (start == pos) {
        hideCompletion();
        return;
    } // cursor not right after an identifier

    const QString token = line.mid(start, pos - start);
    QList<CompletionItemData> cands;
    if (token.startsWith('$')) { // $ namespace is lexically isolated: defines first, then extends
        for (const auto& d : m_staticItems)
            if (d.kind == CompletionItemData::Define && d.name.startsWith(token, Qt::CaseInsensitive)) cands.append(d);
        for (const auto& d : m_staticItems)
            if (d.kind == CompletionItemData::Extend && d.name.startsWith(token, Qt::CaseInsensitive)) cands.append(d);
    } else {
        // keywords / builtins / literals only — user-level candidates (user
        // defs, variables) are a reserved extension point, not implemented
        for (const auto& d : m_staticItems)
            if (d.kind != CompletionItemData::Extend && d.kind != CompletionItemData::Define)
                if (d.name.startsWith(token, Qt::CaseInsensitive)) cands.append(d);
    }
    if (cands.isEmpty()) {
        hideCompletion();
        return;
    }
    if (!Settings::completionShowDesc())
        for (auto& d : cands) d.desc.clear(); // single-row items
    const int listLimit = Settings::completionListLimit(); // 0 = unlimited
    if (listLimit > 0 && cands.size() > listLimit) cands = cands.mid(0, listLimit);
    hideHoverTooltip();
    m_popup->showAt(viewport()->mapToGlobal(cursorRect().bottomLeft()) + QPoint(0, 4), cands,
                    Settings::completionWindowItems());
}

void ScriptEditor::hideCompletion() {
    if (m_popup->isVisible()) m_popup->hide();
    hideHoverTooltip();
}

void ScriptEditor::insertCompletion(bool _asCall) {
    if (!m_popup->isVisible()) return;
    const CompletionItemData d = m_popup->currentData();
    QTextCursor c = textCursor();
    const QString line = c.block().text();
    const int pos = c.positionInBlock();
    auto isId = [](QChar _ch) { return _ch.isLetterOrNumber() || _ch == '_' || _ch == '$'; };
    int start = pos;
    while (start > 0 && isId(line[start - 1])) --start;
    c.setPosition(c.block().position() + start);
    c.setPosition(c.block().position() + pos, QTextCursor::KeepAnchor);
    hideCompletion();
    // three candidate classes: functions (extend + builtin) get the call
    // snippet on Enter; keywords/literals/$defines insert the bare name
    const bool isFunc = d.call && (d.kind == CompletionItemData::Extend || d.kind == CompletionItemData::Builtin ||
                                   d.kind == CompletionItemData::Link || d.kind == CompletionItemData::Import);
    if (!_asCall || !isFunc) {
        c.insertText(d.name);
        return;
    }
    if (d.params.isEmpty()) { // no-arg function / container literal form
        QString call;
        int cursorAt;
        if (d.name == "vec" || d.name == "lst") {
            call = d.name + "[]";
            cursorAt = d.name.size() + 1;
        } else if (d.name == "map") {
            call = d.name + "{}";
            cursorAt = d.name.size() + 1;
        } else {
            call = d.name + "()";
            cursorAt = call.size();
        }
        const int insPos = c.selectionStart(); // insertText advances the anchor too
        c.insertText(call);
        QTextCursor cur(document());
        cur.setPosition(insPos + cursorAt);
        setTextCursor(cur);
        return;
    }
    // VSCode-style call snippet: name(p1, p2) with the first placeholder
    // selected; Tab walks the placeholders
    const QString call = d.name + "(" + d.params.join(", ") + ")";
    const int insPos = c.selectionStart();
    c.insertText(call);
    m_snippetPos.clear();
    int off = d.name.size() + 1;
    for (const QString& p : d.params) {
        m_snippetPos.append({insPos + off, p.size()});
        off += p.size() + 2; // ", "
    }
    m_snippetIdx = 0;
    m_snippetBegin = insPos;
    m_snippetEnd = insPos + call.size();
    QTextCursor cur(document());
    cur.setPosition(m_snippetPos[0].first);
    cur.setPosition(m_snippetPos[0].first + m_snippetPos[0].second, QTextCursor::KeepAnchor);
    setTextCursor(cur);
}

void ScriptEditor::buildStaticItems() {
    auto clean = [](const std::string& _s) {
        QString s = QString::fromStdString(_s).trimmed();
        s.replace('\n', ' ').replace('\r', ' ');
        if (s.size() > 80) s = s.left(77) + "...";
        return s;
    };
    // keyword / literal / builtin words — engine builtin doc from core::document()
    // (keywords.json), same lists as AsHighlighter (lang.md §2.1-2.2)
    const alx::json_object& doc = core::document();
    // name → overloads array (same shape as "functions"); each overload is one
    // candidate row, literals are entries with an empty array
    auto addWordList = [this, &doc](const char* _key, CompletionItemData::Kind _k) {
        if (!doc.contain(_key)) return;
        const auto& words = doc.value(_key).to<alx::json_object>();
        for (auto it = words.begin(); it != words.end(); ++it) {
            const QString name = QString::fromStdString(it.key());
            const auto& ovs = it.value().to<alx::json_array>();
            if (ovs.empty()) { // literal: name only
                m_staticItems.append({name, QString(), QString(), _k, {}, QStringLiteral("base")});
                continue;
            }
            for (const alx::json_value* ov : ovs) {
                const auto& w = ov->to<alx::json_object>();
                QStringList params;
                if (w.contain("params"))
                    for (const alx::json_value* q : w.value("params").to<alx::json_array>())
                        params.append(QString::fromStdString(q->to<std::string>()));
                m_staticItems.append({name,
                                      w.contain("sig") ? QString::fromStdString(w.value("sig").to<std::string>())
                                                       : QString(),
                                      QString(), _k, params, QStringLiteral("base")});
            }
        }
    };
    addWordList("keywords", CompletionItemData::Keyword);
    addWordList("literals", CompletionItemData::Literal);
    addWordList("builtins", CompletionItemData::Builtin);

    // extend functions + define constants — exact list from core::document()
    // (doc keys are bare "math_PI" — the displayed name adds the $ namespace)
    if (doc.contain("functions")) {
        const auto& funcs = doc.value("functions").to<alx::json_object>();
        for (auto it = funcs.begin(); it != funcs.end(); ++it) {
            const QString name = "$" + QString::fromStdString(it.key());
            const auto& ovs = it.value().to<alx::json_array>();
            if (ovs.empty()) { // no overloads: bare name, Enter inserts "()"
                m_staticItems.append({name, "()", QString(), CompletionItemData::Extend, {}, QStringLiteral("base")});
                continue;
            }
            for (const alx::json_value* ov : ovs) { // one candidate row per overload
                const auto& o = ov->to<alx::json_object>();
                QStringList sigParams, params;
                if (o.contain("param"))
                    for (const alx::json_value* p : o.value("param").to<alx::json_array>()) {
                        const auto& po = p->to<alx::json_object>();
                        QString key = QString::fromStdString(po.value("key").to<std::string>());
                        QString type = po.contain("type")
                                           ? QString::fromStdString(po.value("type").to<std::string>())
                                           : QString();
                        QString part = key.endsWith("...")
                                           ? key
                                           : key + (type.isEmpty() ? QString() : ": " + type);
                        if (po.contain("optional") && po.value("optional").to<bool>()) part = "[" + part + "]";
                        sigParams.append(part);
                        params.append(key.endsWith("...") ? key.left(key.size() - 3) : key);
                    }
                QString sig = "(" + sigParams.join(", ") + ")";
                if (o.contain("ret"))
                    sig += " → " + QString::fromStdString(
                                       o.value("ret").to<alx::json_object>().value("type").to<std::string>());
                const QString desc = o.contain("desc") ? clean(o.value("desc").to<std::string>()) : QString();
                m_staticItems.append({name, sig, desc, CompletionItemData::Extend, params, QStringLiteral("base")});
            }
        }
    }
    if (doc.contain("defines")) {
        const auto& defs = doc.value("defines").to<alx::json_object>();
        for (auto it = defs.begin(); it != defs.end(); ++it) {
            const QString name = "$" + QString::fromStdString(it.key());
            const auto& d = it.value().to<alx::json_object>();
            QString sig = d.contain("value")
                              ? "= " + QString::fromStdString(alx::json_doc::to_string(d.value("value"), true))
                              : QString();
            QString desc = d.contain("desc") ? clean(d.value("desc").to<std::string>()) : QString();
            m_staticItems.append({name, sig, desc, CompletionItemData::Define, {}, QStringLiteral("base")});
        }
    }
}

void ScriptEditor::keyPressEvent(QKeyEvent* _ev) {
    // call-snippet placeholder walk: Tab jumps to the next parameter
    if (m_snippetIdx >= 0 && _ev->key() == Qt::Key_Tab) {
        if (++m_snippetIdx < m_snippetPos.size()) {
            const int pos = m_snippetPos[m_snippetIdx].first;
            QTextCursor cur(document());
            cur.setPosition(pos);
            cur.setPosition(pos + m_snippetPos[m_snippetIdx].second, QTextCursor::KeepAnchor);
            setTextCursor(cur);
        } else { // past the last placeholder: leave the snippet, keep the text
            QTextCursor cur = textCursor();
            if (cur.hasSelection()) {
                cur.setPosition(cur.selectionEnd());
                setTextCursor(cur);
            }
            m_snippetPos.clear();
            m_snippetIdx = -1;
        }
        return;
    }
    if (m_popup->isVisible()) {
        switch (_ev->key()) {
        case Qt::Key_Down: m_popup->setCurrentRow((m_popup->currentRow() + 1) % m_popup->count()); return;
        case Qt::Key_Up: m_popup->setCurrentRow((m_popup->currentRow() - 1 + m_popup->count()) % m_popup->count()); return;
        case Qt::Key_Tab: insertCompletion(false); return; // name only
        case Qt::Key_Enter:
        case Qt::Key_Return: insertCompletion(true); return; // name(args) snippet
        case Qt::Key_Escape:
            hideCompletion();
            m_suppress = true;
            return;
        default: break;
        }
    }
    // line ops — table-driven; line.cut/copy/paste default to the same keys as
    // edit.* with VSCode semantics: line op when nothing is selected, plain
    // edit otherwise
    const QKeySequence seq(int(_ev->modifiers()) | _ev->key());
    if (Settings::shortcut("line.cut") == seq) {
        if (textCursor().hasSelection()) cut(); // plain cut over the selection
        else cutLine();
        return;
    }
    if (Settings::shortcut("line.copy") == seq) {
        if (textCursor().hasSelection()) copy();
        else copyLine();
        return;
    }
    if (Settings::shortcut("line.paste") == seq) {
        pasteLine();
        return;
    }
    if (Settings::shortcut("line.moveUp") == seq) {
        moveLines(-1);
        return;
    }
    if (Settings::shortcut("line.moveDown") == seq) {
        moveLines(1);
        return;
    }
    QPlainTextEdit::keyPressEvent(_ev);
}

void ScriptEditor::cutLine() {
    QTextBlock b = textCursor().block();
    QString text = b.text();
    if (b.next().isValid()) text += '\n';
    QApplication::clipboard()->setText(text);
    QTextCursor del(b);
    del.movePosition(QTextCursor::StartOfBlock);
    del.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    if (b.next().isValid()) del.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor); // incl. newline
    del.removeSelectedText();
}

void ScriptEditor::copyLine() {
    QTextBlock b = textCursor().block();
    QString text = b.text();
    if (b.next().isValid()) text += '\n';
    QApplication::clipboard()->setText(text);
}

void ScriptEditor::pasteLine() {
    QTextCursor c = textCursor();
    if (c.hasSelection()) { // plain paste over the selection
        paste();
        return;
    }
    const QString clip = QApplication::clipboard()->text();
    if (!clip.endsWith('\n')) { // not a line clipboard — plain insert
        c.insertText(clip);
        return;
    }
    c.movePosition(QTextCursor::StartOfBlock);
    const int pos = c.position();
    c.insertText(clip);
    QTextCursor cur(document());
    cur.setPosition(pos); // cursor → first line of the pasted block
    setTextCursor(cur);
}

void ScriptEditor::moveLines(int _dir) {
    QTextCursor c = textCursor();
    QTextBlock first = c.block(), last = first;
    if (c.hasSelection()) {
        const int s = qMin(c.selectionStart(), c.selectionEnd());
        const int e = qMax(c.selectionStart(), c.selectionEnd());
        first = document()->findBlock(s);
        last = document()->findBlock(e);
        if (last.position() == e) last = last.previous(); // selection ends at a block start
    }
    QTextBlock peer = _dir < 0 ? first.previous() : last.next();
    if (!peer.isValid()) return; // already at the edge

    // swap the group [first..last] with the adjacent single line
    QTextBlock rangeStart = _dir < 0 ? peer : first;
    QTextBlock rangeEnd = _dir < 0 ? last : peer;
    QTextCursor a(rangeStart);
    a.movePosition(QTextCursor::StartOfBlock);
    QTextCursor b(rangeEnd);
    b.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    if (rangeEnd.next().isValid()) b.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    QTextCursor range(a);
    range.setPosition(b.position(), QTextCursor::KeepAnchor);
    const QString rangeText = range.selectedText(); // T + G (up) / G + T (down)
    QTextCursor g(first);
    g.movePosition(QTextCursor::StartOfBlock);
    QTextCursor ge(last);
    ge.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    if (last.next().isValid()) ge.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    g.setPosition(ge.position(), QTextCursor::KeepAnchor);
    const QString groupText = g.selectedText();
    // rangeText is T + G (up: peer before the group) or G + T (down); T is the
    // single peer line, so strip groupText from the opposite side
    const QString newText = _dir < 0
                                ? groupText + rangeText.left(rangeText.size() - groupText.size())
                                : rangeText.mid(groupText.size()) + groupText;
    // remember the cursor/selection rows + cols — the moved lines keep their
    // row (shifted by ±1) and column
    const int curRow = c.blockNumber(), curCol = c.positionInBlock();
    int selStartRow = -1, selStartCol = -1, selEndRow = -1, selEndCol = -1;
    if (c.hasSelection()) {
        QTextCursor s(c), e(c);
        s.setPosition(qMin(c.selectionStart(), c.selectionEnd()));
        e.setPosition(qMax(c.selectionStart(), c.selectionEnd()));
        selStartRow = s.blockNumber();
        selStartCol = s.positionInBlock();
        selEndRow = e.blockNumber();
        selEndCol = e.positionInBlock();
    }
    range.insertText(newText);
    // restore the cursor/selection after the layout settled (singleShot 0) —
    // setting it right after insertText gets overridden by pending updates
    QTimer::singleShot(0, this,
                       [this, _dir, curRow, curCol, selStartRow, selStartCol, selEndRow, selEndCol] {
                           auto blockAt = [this](int _row, int _col) {
                               QTextBlock b = document()->findBlockByNumber(_row);
                               QTextCursor bc(b);
                               bc.movePosition(QTextCursor::StartOfBlock);
                               bc.movePosition(QTextCursor::NextCharacter, QTextCursor::MoveAnchor,
                                               qMin(_col, b.text().size()));
                               return bc;
                           };
                           QTextCursor cur = blockAt(curRow + _dir, curCol);
                           if (selStartRow >= 0) {
                               const QTextCursor start = blockAt(selStartRow + _dir, selStartCol);
                               const QTextCursor end = blockAt(selEndRow + _dir, selEndCol);
                               cur.setPosition(start.position());
                               cur.setPosition(end.position(), QTextCursor::KeepAnchor);
                           }
                           setTextCursor(cur);
                           ensureCursorVisible();
                       });
}

void ScriptEditor::mousePressEvent(QMouseEvent* _ev) {
    hideCompletion();
    QPlainTextEdit::mousePressEvent(_ev);
}

void ScriptEditor::mouseMoveEvent(QMouseEvent* _ev) {
    QPlainTextEdit::mouseMoveEvent(_ev);
    if (!Settings::hoverEnabled() || m_astMode || isReadOnly()) return;
    if (m_popup->isVisible()) return;

    QTextCursor c = cursorForPosition(_ev->pos());
    const QString line = c.block().text();
    int p = c.positionInBlock();
    auto isId = [](QChar ch) { return ch.isLetterOrNumber() || ch == '_' || ch == '$'; };
    int s = p;
    while (s > 0 && isId(line[s - 1])) --s;
    int e = p;
    while (e < line.size() && isId(line[e])) ++e;
    const QString word = line.mid(s, e - s);

    if (word.isEmpty()) {
        if (!m_hoveredWord.isEmpty()) hideHoverTooltip();
        return;
    }
    if (word == m_hoveredWord) return;
    hideHoverTooltip();
    m_hoveredWord = word;
    m_hoverTimer.start();
}

void ScriptEditor::leaveEvent(QEvent* _ev) {
    QPlainTextEdit::leaveEvent(_ev);
    hideHoverTooltip();
}

void ScriptEditor::hideHoverTooltip() {
    m_hoverTimer.stop();
    m_hoveredWord.clear();
    QToolTip::hideText();
}

QList<CompletionItemData> ScriptEditor::collectMatches(const QString& _word) const {
    QList<CompletionItemData> result;
    for (const auto& d : m_staticItems)
        if (d.name.compare(_word, Qt::CaseInsensitive) == 0) result.append(d);
    return result;
}

void ScriptEditor::inputMethodEvent(QInputMethodEvent* _ev) {
    m_imeComposing = !_ev->preeditString().isEmpty();
    QPlainTextEdit::inputMethodEvent(_ev);
}
