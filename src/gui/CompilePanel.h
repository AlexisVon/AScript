#ifndef COMPILEPANEL_H
#define COMPILEPANEL_H

#include "EmbedWidget.h"

#include <QString>

#include <functional>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

// CompilePanel — ax/axc compile dialog: scope (current file / project / tree),
// debug + embed options, required output directory. Compiles synchronously and
// reports success/failure in-place and through logMessage.
class CompilePanel : public EmbedWidget {
    Q_OBJECT

public:
    // _dir: opened folder (empty = none); _projectAx: active project (empty = none);
    // _currentFile: active editor file; _selectedFile: tree/context target (empty =
    // current file); _treeFirst: default scope is the project/tree.
    CompilePanel(QWidget* parent, const QString& _dir, const QString& _projectAx,
                 const QString& _projectName, const QString& _currentFile,
                 const std::function<bool(const QString&)>& _isDirty, const QString& _selectedFile,
                 bool _treeFirst);

signals:
    void logMessage(const QString& _msg);

private:
    enum Scope { ScopeFile = 0,
                 ScopeProject,
                 ScopeTree };
    void buildUi();
    void runCompile();
    QString configuredOutDir() const; // .ax "out" for the selected scope; empty = choose
    void setResult(const QString& _text, bool _error);

    QString m_dir;
    QString m_projectAx;
    QString m_projectName;
    QString m_currentFile;
    QString m_selectedFile;
    std::function<bool(const QString&)> m_isDirty; // live check at compile time
    bool m_treeFirst;
    QComboBox* m_scope = nullptr;
    QCheckBox* m_debug = nullptr;
    QCheckBox* m_embed = nullptr;
    QLineEdit* m_outDir = nullptr;
    QPlainTextEdit* m_result = nullptr;
    QPushButton* m_runBtn = nullptr;
    bool m_outAuto = true; // field still tracks the configured dir
};

#endif // COMPILEPANEL_H
