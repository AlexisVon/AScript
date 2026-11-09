#ifndef UITERMINAL_H
#define UITERMINAL_H

#include <QString>
#include <QWidget>

class QLineEdit;
class QPlainTextEdit;
class QToolButton;

// UiTerminal — engine console: read-only output + single input line
class UiTerminal : public QWidget {
    Q_OBJECT

public:
    explicit UiTerminal(QWidget* parent = nullptr);

    void applySettings(); // output/input font size from Settings
    void append(const QString& _text);
    void clear();
    void setInputVisible(bool _vis);
    // setLogPath — session log shown in the toolbar; empty shows the off state
    void setLogPath(const QString& _path);

signals:
    void inputEntered(const QString& _line);
    void logPathClicked();

private:
    void updateLogButton();

private:
    QPlainTextEdit* m_output;
    QLineEdit* m_input;
    QToolButton* m_logBtn;
    QString m_logPath;
};
#endif // UITERMINAL_H
