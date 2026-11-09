#include "EditorArea.h"

#include "AsHighlighter.h"
#include "HintProvider.h"
#include "Icons.h"
#include "ScriptEditor.h"
#include "Settings.h"
#include "WelcomePage.h"

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenu>
#include <QMessageBox>
#include <QTabBar>
#include <QTextBlock>
#include <QTextCursor>

namespace {
    // wrap-aware cursor position — visual row within the wrapped line (RegularParse pattern)
    void emit_cursor_pos(EditorArea* _area, QPlainTextEdit* _ed) {
        const QTextCursor c = _ed->textCursor();
        const int col = c.positionInBlock();
        const int row = c.block().layout()->lineForTextPosition(col).lineNumber() + c.block().firstLineNumber();
        emit _area->cursorPosChanged(row + 1, col + 1);
    }
} // namespace

EditorArea::EditorArea(QWidget* parent)
    : QTabWidget(parent) {
    setTabsClosable(true);
    connect(this, &QTabWidget::tabCloseRequested, this, [this](int _index) { closeTab(_index); });
    // tab switch: refresh the status-bar cursor position + file tree sync
    connect(this, &QTabWidget::currentChanged, this, [this] {
        if (auto* ed = currentEditor()) emit_cursor_pos(this, ed);
        emit currentFileChanged(currentPath());
        emit documentsChanged();
    });

    // tab bar context menu: close / close others / close all
    tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tabBar(), &QTabBar::customContextMenuRequested, this, [this](const QPoint& _pos) {
        const int idx = tabBar()->tabAt(_pos);
        if (idx < 0) return;
        QMenu menu(this);
        QAction* closeAct = menu.addAction("关闭");
        QAction* closeOthers = menu.addAction("关闭其他");
        QAction* closeAll = menu.addAction("关闭所有");
        QAction* picked = menu.exec(tabBar()->mapToGlobal(_pos));
        if (picked == closeAct) {
            closeTab(idx);
        } else if (picked == closeOthers) {
            for (int i = count() - 1; i >= 0; --i) {
                if (i == idx) continue; // keep the clicked tab
                if (!closeTab(i)) break; // user cancelled a dirty doc
            }
        } else if (picked == closeAll) {
            for (int i = count() - 1; i >= 0; --i)
                if (!closeTab(i)) break;
        }
    });

    m_welcome = new WelcomePage(this);
    connect(m_welcome, &WelcomePage::newRequested, this, &EditorArea::newRequested);
    connect(m_welcome, &WelcomePage::openFileRequested, this, &EditorArea::openFileRequested);
    connect(m_welcome, &WelcomePage::openDirRequested, this, &EditorArea::openDirRequested);
    connect(m_welcome, &WelcomePage::recentRequested, this, &EditorArea::recentRequested);
    updateWelcome();
}

QList<EditorArea::OpenDoc> EditorArea::openDocuments() const {
    QList<OpenDoc> out;
    for (int i = 0; i < m_docs.size(); ++i) {
        const Doc& d = m_docs[i];
        OpenDoc doc;
        doc.title = tabText(i);
        doc.path = d.filePath.isEmpty() ? d.path : d.filePath;
        doc.dirty = d.dirty;
        doc.readOnly = qobject_cast<ScriptEditor*>(d.editor) == nullptr;
        doc.current = d.editor == currentWidget();
        out.append(doc);
    }
    return out;
}

void EditorArea::activateDocument(int _index) {
    if (_index < 0 || _index >= count()) return;
    setCurrentIndex(_index);
    if (QWidget* w = currentWidget()) w->setFocus();
}

bool EditorArea::closeTab(int _index) {
    QPlainTextEdit* ed = static_cast<QPlainTextEdit*>(widget(_index));
    auto& doc = docOf(ed);
    if (doc.dirty) {
        auto ret = QMessageBox::question(
            this, "未保存", QString("保存 %1 的更改？").arg(doc.path.isEmpty() ? tr("未命名文档") : doc.path));
        if (ret == QMessageBox::Cancel) return false;
        if (ret == QMessageBox::Yes) {
            setCurrentIndex(_index);
            if (!saveCurrent()) return false;
        }
    }
    m_docs.removeAt(_index);
    delete ed;
    updateWelcome();
    emit documentsChanged();
    return true;
}

ScriptEditor* EditorArea::openFile(const QString& _path) {
    for (auto& doc : m_docs) {
        if (doc.path == _path) {
            setCurrentWidget(doc.editor);
            return qobject_cast<ScriptEditor*>(doc.editor);
        }
    }
    ScriptEditor* ed = new ScriptEditor(this);
    ed->setHintProvider(m_hints);
    ed->setFilePath(_path);
    QFile f(_path);
    if (!f.open(QIODevice::ReadOnly)) {
        delete ed;
        return nullptr;
    }
    ed->setPlainText(QString::fromUtf8(f.readAll()));
    if (QFileInfo(_path).suffix() == "json")
        ed->setHighlighter(new AsJsonHighlighter(ed->document())); // JSON files: JSON colors
    m_docs.append({ed, _path, {}, false});
    addTab(ed, QFileInfo(_path).fileName());
    setTabIcon(indexOf(ed), gui::icons::fileIcon(QFileInfo(_path).suffix()));
    setTabToolTip(indexOf(ed), _path);
    connect(ed, &QPlainTextEdit::textChanged, this, [this, ed] {
        auto& doc = docOf(ed);
        doc.dirty = true;
        updateTab(ed);
    });
    connect(ed, &ScriptEditor::runRequested, this, &EditorArea::runRequested);
    connect(ed, &ScriptEditor::astRequested, this, &EditorArea::astRequested);
    connect(ed, &ScriptEditor::formatRequested, this, &EditorArea::formatRequested);
    connect(ed, &ScriptEditor::breakpointToggled, this,
            [this, ed](int _row) { emit breakpointToggled(docOf(ed).path, _row); });
    connect(ed, &QPlainTextEdit::cursorPositionChanged, this, [this, ed] { emit_cursor_pos(this, ed); });
    setCurrentWidget(ed);
    updateWelcome();
    emit documentsChanged();
    return ed;
}

ScriptEditor* EditorArea::newDocument() {
    ScriptEditor* ed = new ScriptEditor(this);
    ed->setHintProvider(m_hints);
    m_docs.append({ed, QString(), {}, false});
    addTab(ed, QString("未命名 %1.axc").arg(m_untitledSeq++));
    setTabIcon(indexOf(ed), gui::icons::fileIcon(QStringLiteral("axc")));
    connect(ed, &QPlainTextEdit::textChanged, this, [this, ed] {
        auto& doc = docOf(ed);
        doc.dirty = true;
        updateTab(ed);
    });
    connect(ed, &ScriptEditor::runRequested, this, &EditorArea::runRequested);
    connect(ed, &ScriptEditor::astRequested, this, &EditorArea::astRequested);
    connect(ed, &ScriptEditor::formatRequested, this, &EditorArea::formatRequested);
    connect(ed, &ScriptEditor::breakpointToggled, this,
            [this, ed](int _row) { emit breakpointToggled(docOf(ed).path, _row); });
    connect(ed, &QPlainTextEdit::cursorPositionChanged, this, [this, ed] { emit_cursor_pos(this, ed); });
    setCurrentWidget(ed);
    updateWelcome();
    emit documentsChanged();
    return ed;
}

ScriptEditor* EditorArea::currentEditor() const {
    return qobject_cast<ScriptEditor*>(currentWidget()); // nullptr on read-only tabs
}

// openReadonly — read-only viewer tab (.axp structure text); reuse by title
void EditorArea::openReadonly(const QString& _title, const QString& _text, ViewKind _kind,
                              const QString& _filePath) {
    for (int i = 0; i < count(); i++) {
        if (tabText(i) == _title) {
            setCurrentIndex(i);
            return;
        }
    }
    auto* view = new QPlainTextEdit(this);
    view->setPlainText(_text);
    view->setReadOnly(true);
    view->setLineWrapMode(QPlainTextEdit::NoWrap);
    view->setFont(QFont(Settings::editorFont(), Settings::editorFontSize())); // global editor font
    // content-type highlighter: AST dump / JSON / plain text
    if (_kind == Ast)
        new AsAstHighlighter(view->document());
    else if (_kind == Json)
        new AsJsonHighlighter(view->document());
    Doc doc;
    doc.editor = view;
    doc.filePath = _filePath;
    m_docs.append(doc);
    const int idx = addTab(view, _title);
    setTabIcon(idx, gui::icons::load(QStringLiteral("lock")));
    setTabToolTip(idx, tr("只读") + (_filePath.isEmpty() ? QString() : QStringLiteral(": ") + _filePath));
    setCurrentWidget(view);
    updateWelcome();
    emit documentsChanged();
}

QString EditorArea::currentPath() {
    auto* w = qobject_cast<QPlainTextEdit*>(currentWidget());
    if (!w) return QString();
    const auto& doc = docOf(w);
    return doc.filePath.isEmpty() ? doc.path : doc.filePath;
}

QString EditorArea::currentTabTitle() {
    return tabText(currentIndex());
}

bool EditorArea::currentDirty() {
    auto* w = qobject_cast<QPlainTextEdit*>(currentWidget());
    return w ? docOf(w).dirty : false;
}

bool EditorArea::isDirty(const QString& _path) const {
    if (_path.isEmpty()) return false;
    const QString abs = QFileInfo(_path).absoluteFilePath();
    for (const auto& d : m_docs) {
        const QString p = d.filePath.isEmpty() ? d.path : d.filePath;
        if (!p.isEmpty() && QFileInfo(p).absoluteFilePath() == abs) return d.dirty;
    }
    return false;
}

bool EditorArea::saveCurrent() {
    ScriptEditor* ed = currentEditor();
    if (!ed) return false;
    auto& doc = docOf(ed);
    if (doc.path.isEmpty()) {
        QString path = QFileDialog::getSaveFileName(this, "保存脚本", QString(), "*.axc");
        if (path.isEmpty()) return false;
        doc.path = path;
        ed->setFilePath(path);
    }
    QFile f(doc.path);
    if (!f.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, "保存失败", "无法写入 " + doc.path);
        return false;
    }
    f.write(ed->toPlainText().toUtf8());
    doc.dirty = false;
    setTabIcon(indexOf(ed), gui::icons::fileIcon(QFileInfo(doc.path).suffix()));
    updateTab(ed);
    return true;
}

void EditorArea::setHintProvider(HintProvider* _provider) {
    m_hints = _provider;
    for (auto& doc : m_docs)
        if (auto* ed = qobject_cast<ScriptEditor*>(doc.editor)) ed->setHintProvider(_provider);
}

QList<QPair<QString, QString>> EditorArea::hintDocuments() const {
    QList<QPair<QString, QString>> out;
    for (const auto& doc : m_docs)
        if (auto* ed = qobject_cast<ScriptEditor*>(doc.editor)) out.append({doc.path, ed->toPlainText()});
    return out;
}

void EditorArea::clearExecutionLines() {
    for (auto& d : m_docs)
        if (auto* ed = qobject_cast<ScriptEditor*>(d.editor)) ed->clearExecutionLine();
}

void EditorArea::applySettings() {
    for (auto& d : m_docs) {
        if (auto* ed = qobject_cast<ScriptEditor*>(d.editor)) {
            ed->applySettings();
        } else { // read-only views follow the global editor font too
            d.editor->setFont(QFont(Settings::editorFont(), Settings::editorFontSize()));
        }
    }
}

EditorArea::Doc& EditorArea::docOf(QPlainTextEdit* _editor) {
    for (auto& doc : m_docs)
        if (doc.editor == _editor) return doc;
    static Doc s_fallback{nullptr, QString(), {}, false}; // unreachable
    return s_fallback;
}

void EditorArea::updateTab(QPlainTextEdit* _editor) {
    auto& doc = docOf(_editor);
    int idx = indexOf(_editor);
    if (idx < 0) return;
    QString title = QFileInfo(doc.path).fileName();
    if (title.isEmpty()) title = tabText(idx);
    if (title.endsWith('*')) title.chop(1);
    if (doc.dirty) title += '*';
    const bool changed = title != tabText(idx);
    setTabText(idx, title);
    if (!doc.path.isEmpty()) setTabToolTip(idx, doc.path);
    if (changed) emit documentsChanged();
}

void EditorArea::updateWelcome() {
    const bool empty = count() == 0;
    tabBar()->setVisible(!empty);
    if (!m_welcome) return;
    m_welcome->setGeometry(rect());
    m_welcome->setVisible(empty);
    if (empty) m_welcome->raise();
}

void EditorArea::resizeEvent(QResizeEvent* _ev) {
    QTabWidget::resizeEvent(_ev);
    if (m_welcome) m_welcome->setGeometry(rect());
}
