#ifndef OPENFILESPANEL_H
#define OPENFILESPANEL_H

#include "EditorArea.h"

#include <QWidget>

class QLabel;
class QListWidget;

// OpenFilesPanel — files dock top section: open tabs in tab order, dirty marker,
// lock icon for read-only views; click activates the tab.
class OpenFilesPanel : public QWidget {
    Q_OBJECT

public:
    explicit OpenFilesPanel(QWidget* parent = nullptr);
    void setDocuments(const QList<EditorArea::OpenDoc>& _docs);
    int preferredHeight() const; // header + rows (1–6), for the files splitter

signals:
    void activateRequested(int _index);
    void runRequested(const QString& _path);
    void runDebugRequested(const QString& _path);
    void compileRequested(const QString& _path);
    void closeRequested(int _index);

private:
    QLabel* m_header;
    QListWidget* m_list;
};

#endif // OPENFILESPANEL_H
