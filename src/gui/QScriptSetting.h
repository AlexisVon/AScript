#ifndef QSCRIPTSETTING_H
#define QSCRIPTSETTING_H

#include "EmbedWidget.h"

#include <QList>
#include <QPair>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QSpinBox;
class QTableWidget;
class QToolButton;

namespace Ui {
    class QScriptSetting;
}

// QScriptSetting — modal settings panel (EmbedWidget convention): left nav
// buttons + stacked pages for editor font, output size, the full shortcut
// table (search + conflict check) and completion params. Save writes QSettings
// and emits settingsApplied for QScriptMain to refresh every widget.
class QScriptSetting : public EmbedWidget {
    Q_OBJECT

public:
    explicit QScriptSetting(QWidget* parent = nullptr);
    ~QScriptSetting();

signals:
    void settingsApplied(); // saved — QScriptMain refreshes editors/menus/terminal

protected:
    void showEvent(QShowEvent* _ev) override; // modal: block the parent
    void hideEvent(QHideEvent* _ev) override; // re-enable the parent on close

private:
    void buildPages(); // nav buttons + stacked pages (ui keeps the skeleton)
    void loadValues(); // QSettings → controls
    void saveValues(); // controls → QSettings, conflict check before writing
    void resetAll(); // all controls back to defaults (saved on the next save)
    void update_frame_hint(); // throughput / memory hint under the frame rows
    void populateShortcutTable(const QString& _filter); // table rows by name filter
    static bool shortcutConflicts(const QList<QPair<QString, QKeySequence>>& _all,
                                  QList<QPair<QString, QString>>& _out); // excludes line.*/edit.* pairs

    Ui::QScriptSetting* ui;
    QWidget* m_mask = nullptr; // dim overlay while open (lock-window pattern)
    QToolButton* m_navButtons[6]; // 编辑器/输出/快捷键/代码提示/运行/库
    // editor page
    QComboBox* m_fontBox = nullptr;
    QSpinBox* m_fontSize = nullptr;
    QComboBox* m_uiScale = nullptr; // whole-UI scale; restart to apply
    // output page
    QSpinBox* m_outputSize = nullptr;
    // shortcut page
    QLineEdit* m_searchEdit = nullptr;
    QTableWidget* m_shortcutTable = nullptr;
    // completion page
    QCheckBox* m_cbEnabled = nullptr;
    QSpinBox* m_cbDelay = nullptr;
    QSpinBox* m_cbWindowItems = nullptr; // popup height in rows (min 4)
    QSpinBox* m_cbListLimit = nullptr; // total candidate cap, 0 = unlimited
    QSpinBox* m_cbCacheLimit = nullptr; // link/import doc entries, 0 = unlimited
    QCheckBox* m_cbShowDesc = nullptr;
    QSpinBox* m_completionSize = nullptr; // completion popup font size
    QCheckBox* m_cbHover = nullptr;
    QSpinBox* m_hoverSize = nullptr; // hover tooltip font size
    // library page
    QListWidget* m_sysLibs = nullptr; // read-only host dirs
    QListWidget* m_userLibs = nullptr; // editable expressions
    // run page
    QCheckBox* m_runnerEnabled = nullptr;
    QLineEdit* m_runnerPath = nullptr;
    QSpinBox* m_runWall = nullptr;
    QSpinBox* m_runMem = nullptr;
    QCheckBox* m_debugOn = nullptr;
    QCheckBox* m_mtDebug = nullptr;
    QSpinBox* m_logKeep = nullptr;
    QCheckBox* m_frameMsOn = nullptr;
    QSpinBox* m_frameMs = nullptr;
    QCheckBox* m_frameBytesOn = nullptr;
    QSpinBox* m_frameBytes = nullptr;
    QCheckBox* m_trimBytesOn = nullptr;
    QSpinBox* m_trimBytes = nullptr;
    QCheckBox* m_logFile = nullptr;
    QLabel* m_frameHint = nullptr;
};

#endif // QSCRIPTSETTING_H
