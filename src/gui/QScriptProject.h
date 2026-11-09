#ifndef QSCRIPTPROJECT_H
#define QSCRIPTPROJECT_H

#include "EmbedWidget.h"

#include <QString>

class QListWidget;

namespace Ui {
    class QScriptProject;
}

// QScriptProject — .ax project config form (EmbedWidget + .ui controls, same
// architecture as QScriptSetting): dim mask while open, fields for name / main
// (axc picker, required) / engine params / path list / hint file; save writes
// the .ax JSON back (relative paths, non-default keys only). Edit mode:
// existing .ax parsed → prefilled; new mode (axPath empty): save prompts for
// location.
class QScriptProject : public EmbedWidget {
    Q_OBJECT

public:
    explicit QScriptProject(QWidget* parent = nullptr, const QString& _axPath = QString());
    ~QScriptProject();

    // target .ax for the next open (empty = new project form)
    void setAxPath(const QString& _path) { m_axPath = _path; }

    // mark the current .ax as form-created: closing without a save (file still
    // empty) removes it — the "new" flow's cancel cleans up the empty file
    void markCreated(bool _created) { m_createdByForm = _created; }

signals:
    void saved(const QString& _axPath); // written — callers refresh views

protected:
    void showEvent(QShowEvent* _ev) override; // modal: block the parent
    void hideEvent(QHideEvent* _ev) override; // re-enable the parent on close

private:
    void loadValues(); // parse .ax → controls (new mode: blanks)
    void updatePathPlaceholder(QListWidget* _list); // grey hint item while a path list is empty
    void saveValues(); // controls → .ax JSON, main required before writing
    void populateMainList(); // recursive *.axc under the project dir

    Ui::QScriptProject* ui;
    QWidget* m_mask = nullptr; // dim overlay while open (lock-window pattern)
    QString m_axPath; // empty = new project (save prompts for a location)
    bool m_createdByForm = false; // cancel on an empty file removes it
};

#endif // QSCRIPTPROJECT_H
