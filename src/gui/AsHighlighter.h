#ifndef ASHIGHLIGHTER_H
#define ASHIGHLIGHTER_H

#include <QSyntaxHighlighter>
#include <QRegularExpression>
#include <QTextCharFormat>

#include <QVector>

// AsHighlighter — AScript syntax highlighter (dark theme, VSCode Dark+ palette).
// Later rules override earlier ones, so comments (applied last) win over
// strings/keywords inside them; block comments additionally override via
// setCurrentBlockState (1 = inside /* */).
class AsHighlighter : public QSyntaxHighlighter {
    Q_OBJECT

public:
    explicit AsHighlighter(QTextDocument* _parent = nullptr);

protected:
    void highlightBlock(const QString& _text) override;

private:
    struct Rule {
        QRegularExpression pattern;
        QTextCharFormat format;
    };
    QVector<Rule> m_rules;
    QTextCharFormat m_commentFormat;
    static const int kInBlockComment = 1;
};

// AsAstHighlighter — colors the prtast dump: node names, posmaps, strings, numbers.
// Later rules override earlier ones — strings/posmaps last so digits inside stay uncolored.
class AsAstHighlighter : public QSyntaxHighlighter {
    Q_OBJECT

public:
    explicit AsAstHighlighter(QTextDocument* _doc = nullptr);

protected:
    void highlightBlock(const QString& _text) override;

private:
    struct Rule {
        QRegularExpression pattern;
        QTextCharFormat format;
    };
    QVector<Rule> m_rules;
};

// AsJsonHighlighter — hint JSON: keys, strings, numbers, true/false/null.
class AsJsonHighlighter : public QSyntaxHighlighter {
    Q_OBJECT

public:
    explicit AsJsonHighlighter(QTextDocument* _doc = nullptr);

protected:
    void highlightBlock(const QString& _text) override;

private:
    struct Rule {
        QRegularExpression pattern;
        QTextCharFormat format;
    };
    QVector<Rule> m_rules;
};
#endif // ASHIGHLIGHTER_H
