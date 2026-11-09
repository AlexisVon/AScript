#include "AsHighlighter.h"

#include <QColor>
#include <QFont>

namespace {

    // gruvbox dark palette (matches the user's vim colorscheme)
    QTextCharFormat make_format(const QColor& _color, bool _bold = false, bool _italic = false) {
        QTextCharFormat f;
        f.setForeground(_color);
        if (_bold) f.setFontWeight(QFont::Bold);
        if (_italic) f.setFontItalic(true);
        return f;
    }

} // namespace

AsHighlighter::AsHighlighter(QTextDocument* _parent)
    : QSyntaxHighlighter(_parent) {
    const QColor red(0xfb, 0x49, 0x34); // Statement — keywords
    const QColor orange(0xfe, 0x80, 0x19); // Constant/Number — literals, numbers
    const QColor purple(0xd3, 0x86, 0x9b); // Special — builtin functions
    const QColor aqua(0x8e, 0xc0, 0x7c); // PreProc — $xxx extends
    const QColor green(0xb8, 0xbb, 0x26); // String
    const QColor gray(0x92, 0x83, 0x74); // Comment

    // lang.md §2.1: 19 keywords
    m_rules.append({QRegularExpression(
                        "\\b(var|def|if|else|while|for|break|continue|return|"
                        "import|link|as|try|catch|throw|switch|case|default|delete)\\b"),
                    make_format(red, true)});
    // 5 literals
    m_rules.append({QRegularExpression("\\b(true|false|null|nan|inf)\\b"), make_format(orange)});
    // 14 builtin functions (reserved, non-identifier)
    m_rules.append({QRegularExpression(
                        "\\b(int|float|string|bool|bytes|vec|map|lst|type|env|here|eval|trap|yield)\\b"),
                    make_format(purple)});
    // $xxx extends — lexical namespace, wildcard pattern (exact list from doc JSON in completion)
    // case: extend names are lowercase, define constants uppercase ($math_PI)
    m_rules.append({QRegularExpression("\\$[a-zA-Z][a-zA-Z0-9_]*"), make_format(aqua)});
    // numbers: hex/oct/bin/dec/float/exponent (lang.md §3.8)
    m_rules.append({QRegularExpression(
                        "\\b(0[xX][0-9a-fA-F]+|0[oO][0-7]+|0[bB][01]+|"
                        "\\d+(\\.\\d+)?([eE][+-]?\\d+)?)\\b"),
                    make_format(orange)});
    // strings: "escaped" 'single' `raw`
    m_rules.append({QRegularExpression(
                        "\"(\\\\.|[^\"\\\\])*\"|'(\\\\.|[^'\\\\])*'|`[^`]*`"),
                    make_format(green)});
    // comments: single-line rule last, block comments handled below
    m_commentFormat = make_format(gray, false, true);
    m_rules.append({QRegularExpression("//[^\n]*"), m_commentFormat});
}

void AsHighlighter::highlightBlock(const QString& _text) {
    // lexically specific rules first; comments override them
    for (const auto& rule : m_rules) {
        auto it = rule.pattern.globalMatch(_text);
        while (it.hasNext()) {
            auto match = it.next();
            setFormat(match.capturedStart(), match.capturedLength(), rule.format);
        }
    }

    // /* */ block comment spans lines (not nested, lang.md §2 note)
    setCurrentBlockState(0);
    int start = 0;
    if (previousBlockState() == kInBlockComment) {
        int close = _text.indexOf("*/");
        if (close == -1) { // still inside the comment
            setFormat(0, _text.length(), m_commentFormat);
            setCurrentBlockState(kInBlockComment);
            return;
        }
        setFormat(0, close + 2, m_commentFormat);
        start = close + 2;
    }
    while (true) {
        int open = _text.indexOf("/*", start);
        if (open == -1) break;
        int close = _text.indexOf("*/", open + 2);
        if (close == -1) { // comment runs to end of line, continues next block
            setFormat(open, _text.length() - open, m_commentFormat);
            setCurrentBlockState(kInBlockComment);
            break;
        }
        setFormat(open, close + 2 - open, m_commentFormat);
        start = close + 2;
    }
}

// ── AsAstHighlighter — prtast dump: node names, posmaps, strings, numbers ──
AsAstHighlighter::AsAstHighlighter(QTextDocument* _doc)
    : QSyntaxHighlighter(_doc) {
    const QColor aqua(0x8e, 0xc0, 0x7c); // node names
    const QColor orange(0xfe, 0x80, 0x19); // numbers
    const QColor green(0xb8, 0xbb, 0x26); // strings
    const QColor gray(0x92, 0x83, 0x74); // posmap
    // order matters: later rules override earlier ones, so strings and
    // posmaps come last — digits inside them must not turn orange
    m_rules.append({QRegularExpression(R"(\([A-Z_]+)"), make_format(aqua)});
    m_rules.append({QRegularExpression(R"(\b\d+\b)"), make_format(orange)});
    m_rules.append({QRegularExpression(R"("[^"]*")"), make_format(green)});
    m_rules.append({QRegularExpression(R"(\{[^}]*\})"), make_format(gray)});
}

void AsAstHighlighter::highlightBlock(const QString& _text) {
    for (const auto& r : m_rules) {
        auto it = r.pattern.globalMatch(_text);
        while (it.hasNext()) {
            const auto m = it.next();
            setFormat(m.capturedStart(), m.capturedLength(), r.format);
        }
    }
}

// ── AsJsonHighlighter — keys, strings, numbers, true/false/null ──
AsJsonHighlighter::AsJsonHighlighter(QTextDocument* _doc)
    : QSyntaxHighlighter(_doc) {
    const QColor aqua(0x8e, 0xc0, 0x7c); // keys
    const QColor green(0xb8, 0xbb, 0x26); // strings
    const QColor orange(0xfe, 0x80, 0x19); // numbers
    const QColor purple(0xd3, 0x86, 0x9b); // true/false/null
    // key = string directly before ':' — match first so it wins over plain strings
    m_rules.append({QRegularExpression(R"("[^"]*"(?=\s*:))"), make_format(aqua)});
    m_rules.append({QRegularExpression(R"("[^"]*")"), make_format(green)});
    m_rules.append({QRegularExpression(R"(\b\d+(\.\d+)?([eE][+-]?\d+)?\b)"), make_format(orange)});
    m_rules.append({QRegularExpression(R"(\b(true|false|null)\b)"), make_format(purple)});
}

void AsJsonHighlighter::highlightBlock(const QString& _text) {
    for (const auto& r : m_rules) {
        auto it = r.pattern.globalMatch(_text);
        while (it.hasNext()) {
            const auto m = it.next();
            setFormat(m.capturedStart(), m.capturedLength(), r.format);
        }
    }
}
