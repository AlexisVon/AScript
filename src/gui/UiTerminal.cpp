#include "UiTerminal.h"

#include "Icons.h"
#include "Settings.h"

#include <QClipboard>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextCursor>
#include <QTextDocument>
#include <QToolButton>
#include <QVBoxLayout>

UiTerminal::UiTerminal(QWidget* parent)
    : QWidget(parent), m_output(new QPlainTextEdit(this)), m_input(new QLineEdit(this)) {
    m_output->setReadOnly(true);
    m_output->setMaximumBlockCount(10000);
    m_output->document()->setDocumentMargin(6);
    m_input->setFixedHeight(24); // uniform input height (QSS lacks min-height)
    m_input->setPlaceholderText("输入（$io_input 等待时可用）");
    m_input->hide(); // hidden until inputRequested

    auto* copyBtn = new QToolButton(this);
    copyBtn->setIcon(gui::icons::load(QStringLiteral("copy")));
    copyBtn->setIconSize(QSize(14, 14));
    copyBtn->setAutoRaise(true);
    copyBtn->setToolTip(tr("复制全部输出"));
    copyBtn->setCursor(Qt::PointingHandCursor);
    connect(copyBtn, &QToolButton::clicked, this, [this] {
        QGuiApplication::clipboard()->setText(m_output->toPlainText());
    });
    auto* clearBtn = new QToolButton(this);
    clearBtn->setIcon(gui::icons::load(QStringLiteral("trash")));
    clearBtn->setIconSize(QSize(14, 14));
    clearBtn->setAutoRaise(true);
    clearBtn->setToolTip(tr("清空输出"));
    clearBtn->setCursor(Qt::PointingHandCursor);
    connect(clearBtn, &QToolButton::clicked, m_output, &QPlainTextEdit::clear);

    m_logBtn = new QToolButton(this);
    m_logBtn->setAutoRaise(true);
    m_logBtn->setCursor(Qt::PointingHandCursor);
    m_logBtn->setEnabled(false);
    connect(m_logBtn, &QToolButton::clicked, this, &UiTerminal::logPathClicked);
    updateLogButton();

    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(6, 2, 6, 0);
    bar->setSpacing(2);
    bar->addWidget(m_logBtn);
    bar->addStretch();
    bar->addWidget(copyBtn);
    bar->addWidget(clearBtn);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(2);
    lay->addLayout(bar);
    lay->addWidget(m_output);
    lay->addWidget(m_input);

    connect(m_input, &QLineEdit::returnPressed, this, [this] {
        emit inputEntered(m_input->text());
        m_input->clear();
        m_input->hide();
    });
    applySettings();
}

void UiTerminal::applySettings() {
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    f.setPointSize(Settings::outputFontSize());
    m_output->setFont(f);
    m_input->setFont(f);
}

void UiTerminal::append(const QString& _text) {
    QScrollBar* sb = m_output->verticalScrollBar();
    const bool atBottom = sb->value() >= sb->maximum() - 2;
    const int keep = sb->value();
    const int before = m_output->blockCount();
    QTextCursor c(m_output->document()); // insert without moving the visible cursor
    c.movePosition(QTextCursor::End);
    c.insertText(_text);
    // maximumBlockCount trims the oldest blocks: shift the kept offset by their height
    const int inserted = static_cast<int>(_text.count(QLatin1Char('\n')));
    const int trimmed = qMax(0, before + inserted - m_output->blockCount());
    const int shift = trimmed * m_output->fontMetrics().lineSpacing();
    sb->setValue(atBottom ? sb->maximum() : qMax(0, keep - shift));
}

void UiTerminal::clear() {
    m_output->clear();
}

void UiTerminal::setInputVisible(bool _vis) {
    if (_vis) {
        m_input->show();
        m_input->setFocus();
    } else {
        m_input->hide();
    }
}

void UiTerminal::setLogPath(const QString& _path) {
    m_logPath = _path;
    updateLogButton();
}

void UiTerminal::updateLogButton() {
    if (m_logPath.isEmpty()) {
        m_logBtn->setText(tr("日志未写入文件"));
        m_logBtn->setToolTip(tr("当前设置不写会话日志"));
        m_logBtn->setEnabled(false);
        return;
    }
    m_logBtn->setText(m_logBtn->fontMetrics().elidedText(m_logPath, Qt::ElideMiddle, 280));
    m_logBtn->setToolTip(m_logPath);
    m_logBtn->setEnabled(true);
}
