#ifndef WELCOMEPAGE_H
#define WELCOMEPAGE_H

#include <QWidget>

class QListWidget;
class QShowEvent;

// WelcomePage — empty-editor state: new/open entries, recent files, key hints.
class WelcomePage : public QWidget {
    Q_OBJECT

public:
    explicit WelcomePage(QWidget* parent = nullptr);
    void refreshRecent();

signals:
    void newRequested();
    void openFileRequested();
    void openDirRequested();
    void recentRequested(const QString& _path);

protected:
    void showEvent(QShowEvent* _ev) override;

private:
    QListWidget* m_recent;
};

#endif // WELCOMEPAGE_H
