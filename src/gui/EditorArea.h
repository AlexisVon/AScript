#ifndef EDITORAREA_H
#define EDITORAREA_H

#include <QPair>
#include <QPlainTextEdit>
#include <QTabWidget>

class HintProvider;
class QResizeEvent;
class ScriptEditor;
class WelcomePage;

class EditorArea : public QTabWidget {
    Q_OBJECT

public:
    explicit EditorArea(QWidget* parent = nullptr);

    struct OpenDoc {
        QString title;
        QString path;
        bool dirty = false;
        bool readOnly = false;
        bool current = false;
    };
    QList<OpenDoc> openDocuments() const; // tab order, for the files dock list
    void activateDocument(int _index);
    bool closeDocument(int _index) { return closeTab(_index); } // close via the files dock

    ScriptEditor* openFile(const QString& _path); // reuse open tab, else create
    ScriptEditor* newDocument();
    ScriptEditor* currentEditor() const; // nullptr on a read-only tab
    // read-only viewer tab (.axp texts); _kind picks the highlighter
    enum ViewKind { Plain,
                    Ast,
                    Json };
    void openReadonly(const QString& _title, const QString& _text, ViewKind _kind,
                      const QString& _filePath = QString());
    QString currentPath(); // empty = untitled
    QString currentTabTitle(); // tab text (e.g. "dev.axp: [::]")
    bool currentDirty();
    bool isDirty(const QString& _path) const; // any open doc with this path
    bool saveCurrent(); // false on cancel/failure
    void setHintProvider(HintProvider* _provider); // link/import docs for every editor
    QList<QPair<QString, QString>> hintDocuments() const; // {filePath, text} for cache reload
    void clearExecutionLines(); // clear the paused-line highlight in every doc
    void applySettings(); // font + completion params for every open editor

signals:
    void runRequested(); // forwarded from the editor context menu
    void astRequested();
    void formatRequested(); // "格式化" from the editor context menu
    void cursorPosChanged(int _row, int _col); // current editor cursor, 1-based wrap-aware
    void currentFileChanged(const QString& _path); // tab switch — for file tree sync
    void breakpointToggled(const QString& _path, int _row); // gutter click on an editable doc
    void documentsChanged(); // open/close/title/current — refresh the files dock list
    void newRequested(); // welcome page
    void openFileRequested();
    void openDirRequested();
    void recentRequested(const QString& _path);

protected:
    void resizeEvent(QResizeEvent* _ev) override;

private:
    bool closeTab(int _index); // dirty confirmation; false = cancelled

    struct Doc {
        QPlainTextEdit* editor; // ScriptEditor (editable) or read-only view
        QString path; // save path (empty = untitled)
        QString filePath; // full path for readonly tabs (.axp), used by sync
        bool dirty = false;
    };
    Doc& docOf(QPlainTextEdit* _editor);
    void updateTab(QPlainTextEdit* _editor);
    void updateWelcome(); // empty state visibility + geometry

    QList<Doc> m_docs;
    WelcomePage* m_welcome = nullptr;
    HintProvider* m_hints = nullptr;
    int m_untitledSeq = 1;
};
#endif // EDITORAREA_H
