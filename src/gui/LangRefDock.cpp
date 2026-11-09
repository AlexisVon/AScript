#include "LangRefDock.h"

#include "Settings.h"
#include "core/core.h"

#include "HintProvider.h"

#include <QFont>
#include <QLineEdit>
#include <QScrollBar>
#include <QSet>
#include <QSplitter>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>
#include <functional>

namespace {

    constexpr int kKind = Qt::UserRole;
    constexpr int kData = Qt::UserRole + 1;
    constexpr int kBlock = Qt::UserRole + 2;

    enum Kind { KRoot = -1,
                KChapter = 0,
                KSection = 1,
                KFunction = 2,
                KModule = 3,
                KLibraryLink = 4,
                KLibraryImport = 5 };

    QString esc(const QString& _s) {
        return _s.toHtmlEscaped();
    }

    QString str(const alx::json_object& _o, const char* _key) {
        return _o.contain(_key) ? QString::fromStdString(_o.value(_key).to<std::string>()) : QString();
    }

    long long jint(const alx::json_object& _o, const char* _key, long long _def = 0) {
        return _o.contain(_key) ? _o.value(_key).to_intg(_def) : _def;
    }

    // Inline markup: `code` and **bold** (bold may contain code); everything else literal.
    QString inlineHtml(const QString& _s) {
        QString out, plain;
        auto flush = [&] {
            if (plain.isEmpty()) return;
            out += esc(plain);
            plain.clear();
        };
        int i = 0;
        while (i < _s.size()) {
            if (_s[i] == QLatin1Char('`')) {
                const int j = _s.indexOf(QLatin1Char('`'), i + 1);
                if (j < 0) {
                    plain += _s.mid(i);
                    break;
                }
                flush();
                out += QStringLiteral("<code>") + esc(_s.mid(i + 1, j - i - 1)) +
                       QStringLiteral("</code>");
                i = j + 1;
                continue;
            }
            if (_s.mid(i, 2) == QLatin1String("**")) {
                const int j = _s.indexOf(QLatin1String("**"), i + 2);
                if (j < 0) {
                    plain += _s.mid(i);
                    break;
                }
                flush();
                out += QStringLiteral("<b>") + inlineHtml(_s.mid(i + 2, j - i - 2)) +
                       QStringLiteral("</b>");
                i = j + 2;
                continue;
            }
            plain += _s[i];
            ++i;
        }
        flush();
        return out;
    }

    QString inlinePlain(const QString& _s) {
        QString out;
        int i = 0;
        while (i < _s.size()) {
            if (_s[i] == QLatin1Char('`')) {
                const int j = _s.indexOf(QLatin1Char('`'), i + 1);
                if (j < 0) {
                    out += _s.mid(i);
                    break;
                }
                out += _s.mid(i + 1, j - i - 1);
                i = j + 1;
                continue;
            }
            if (_s.mid(i, 2) == QLatin1String("**")) {
                const int j = _s.indexOf(QLatin1String("**"), i + 2);
                if (j < 0) {
                    out += _s.mid(i);
                    break;
                }
                out += inlinePlain(_s.mid(i + 2, j - i - 2));
                i = j + 2;
                continue;
            }
            out += _s[i];
            ++i;
        }
        return out;
    }

    QString jtext(const alx::json_value& _v) {
        return QString::fromStdString(_v.to<std::string>());
    }

    QString blockHtml(const alx::json_value& _value, int _index) {
        const auto& b = _value.to<alx::json_object>();
        const QString type = str(b, "type");
        const QString anchor = QStringLiteral("<a name=\"b%1\"></a>").arg(_index);

        if (type == QLatin1String("heading")) {
            const int tag = std::clamp(static_cast<int>(jint(b, "level", 1)), 1, 3);
            return anchor + QStringLiteral("<h%1>%2</h%1>").arg(tag).arg(inlineHtml(str(b, "text")));
        }
        if (type == QLatin1String("paragraph"))
            return QStringLiteral("<p>%1</p>").arg(inlineHtml(str(b, "text")));
        if (type == QLatin1String("quote"))
            return QStringLiteral("<p style=\"margin-left:24px\">%1</p>")
                .arg(inlineHtml(str(b, "text")));
        if (type == QLatin1String("code")) {
            QStringList lines;
            for (const alx::json_value* line : b.value("lines").to<alx::json_array>())
                lines.append(esc(jtext(*line)));
            return QStringLiteral("<pre>%1</pre>").arg(lines.join(QLatin1Char('\n')));
        }
        if (type == QLatin1String("table")) {
            const auto& rows = b.value("rows").to<alx::json_array>();
            QStringList align;
            if (b.contain("align"))
                for (const alx::json_value* a : b.value("align").to<alx::json_array>())
                    align.append(jtext(*a));
            QString html = QStringLiteral("<table border=\"1\" cellspacing=\"0\" cellpadding=\"4\" "
                                          "width=\"100%\">");
            for (int r = 0; r < static_cast<int>(rows.size()); ++r) {
                const auto& cells = rows[r].to<alx::json_array>();
                html += QStringLiteral("<tr>");
                for (int c = 0; c < static_cast<int>(cells.size()); ++c) {
                    const QString tag = r == 0 ? QStringLiteral("th") : QStringLiteral("td");
                    const QString attr = c < align.size()
                                             ? QStringLiteral(" align=\"%1\"").arg(align[c])
                                             : QString();
                    html += QStringLiteral("<%1%2>%3</%1>").arg(tag, attr, inlineHtml(jtext(cells[c])));
                }
                html += QStringLiteral("</tr>");
            }
            return html + QStringLiteral("</table>");
        }
        if (type == QLatin1String("list")) {
            const bool ordered = b.contain("ordered") && b.value("ordered").to_bool();
            const QString tag = ordered ? QStringLiteral("ol") : QStringLiteral("ul");
            QString html = QStringLiteral("<%1>").arg(tag);
            for (const alx::json_value* item : b.value("items").to<alx::json_array>())
                html += QStringLiteral("<li>%1</li>").arg(inlineHtml(jtext(*item)));
            return html + QStringLiteral("</%1>").arg(tag);
        }

        Q_ASSERT_X(false, "LangRefDock::blockHtml", "unknown block type");
        return QStringLiteral("<p style=\"color:#fb4934\">未知块类型：%1</p>").arg(esc(type));
    }

    QString paramText(const alx::json_object& _p) {
        QString part = str(_p, "key");
        const QString type = str(_p, "type");
        if (!type.isEmpty() && !part.endsWith("...")) part += ": " + type;
        if (_p.contain("default"))
            part += " = " + QString::fromStdString(alx::json_doc::to_string(_p.value("default"), true));
        if (_p.contain("optional") && _p.value("optional").to<bool>()) part = "[" + part + "]";
        return part;
    }

    QString signatureOf(const QString& _key, const alx::json_object& _overload) {
        QStringList parts;
        if (_overload.contain("param"))
            for (const alx::json_value* p : _overload.value("param").to<alx::json_array>())
                parts.append(paramText(p->to<alx::json_object>()));
        QString sig = "$" + _key + "(" + parts.join(", ") + ")";
        if (_overload.contain("ret")) {
            const auto& ret = _overload.value("ret").to<alx::json_object>();
            if (ret.contain("type")) sig += " → " + str(ret, "type");
        }
        return sig;
    }

    QString paramRows(const alx::json_object& _overload) {
        if (!_overload.contain("param")) return QString();
        QString rows;
        for (const alx::json_value* p : _overload.value("param").to<alx::json_array>()) {
            const auto& po = p->to<alx::json_object>();
            QString meta = str(po, "key");
            const QString type = str(po, "type");
            if (!type.isEmpty()) meta += ": " + type;
            if (po.contain("default"))
                meta += " = " +
                        QString::fromStdString(alx::json_doc::to_string(po.value("default"), true));
            if (po.contain("optional") && po.value("optional").to<bool>()) meta += " (optional)";
            rows += "<tr><td style=\"color:#ebdbb2\"><span style=\"font-family:'Fira Code'\">" +
                    esc(meta) + "</span></td><td style=\"color:#a89984\">" + esc(str(po, "desc")) +
                    "</td></tr>";
        }
        return rows;
    }

    QString overloadHtml(const QString& _key, const alx::json_object& _overload) {
        QString html = "<p style=\"font-family:'Fira Code';color:#83a598\">" +
                       esc(signatureOf(_key, _overload)) + "</p>";
        const QString desc = str(_overload, "desc");
        if (!desc.isEmpty()) html += "<p style=\"color:#ebdbb2\">" + esc(desc) + "</p>";
        const QString rows = paramRows(_overload);
        if (!rows.isEmpty())
            html += "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"3\">" + rows + "</table>";
        if (_overload.contain("ret")) {
            const auto& ret = _overload.value("ret").to<alx::json_object>();
            const QString retDesc = str(ret, "desc");
            html += "<p style=\"color:#d5c4a1\">→ <span style=\"font-family:'Fira Code'\">" +
                    esc(str(ret, "type")) + "</span>" +
                    (retDesc.isEmpty() ? QString() : " — " + esc(retDesc)) + "</p>";
        }
        return html;
    }

    QString defineHtml(const QString& _key, const alx::json_object& _def) {
        QString html = "<h2 style=\"color:#fbf1c7\">$" + esc(_key) + "</h2>";
        const QString type = str(_def, "type");
        QString value;
        if (_def.contain("value"))
            value = QString::fromStdString(alx::json_doc::to_string(_def.value("value"), true));
        html += "<p style=\"font-family:'Fira Code';color:#83a598\">" + esc(type) +
                (value.isEmpty() ? QString() : " = " + esc(value)) + "</p>";
        const QString desc = str(_def, "desc");
        if (!desc.isEmpty()) html += "<p style=\"color:#ebdbb2\">" + esc(desc) + "</p>";
        return html;
    }

    QString functionHtml(const QString& _key, const alx::json_object& _doc) {
        if (_doc.contain("functions")) {
            const auto& funcs = _doc.value("functions").to<alx::json_object>();
            if (funcs.contain(_key.toStdString())) {
                const auto& ovs = funcs.value(_key.toStdString()).to<alx::json_array>();
                QString html = "<h2 style=\"color:#fbf1c7\">$" + esc(_key) + "</h2>";
                if (ovs.empty())
                    html += "<p style=\"font-family:'Fira Code';color:#83a598\">$" + esc(_key) + "()</p>";
                int n = 0;
                for (const alx::json_value* ov : ovs) {
                    if (n++) html += "<hr style=\"border:none;border-top:1px solid #504945\">";
                    html += overloadHtml(_key, ov->to<alx::json_object>());
                }
                return html;
            }
        }
        if (_doc.contain("defines")) {
            const auto& defs = _doc.value("defines").to<alx::json_object>();
            if (defs.contain(_key.toStdString()))
                return defineHtml(_key, defs.value(_key.toStdString()).to<alx::json_object>());
        }
        return "<p style=\"color:#928374\">unknown: $" + esc(_key) + "</p>";
    }

    QString searchTextOf(const QString& _key, const alx::json_object& _doc) {
        QStringList parts{_key, "$" + _key};
        auto add = [&parts](const alx::json_object& _o, const char* _k) {
            const QString s = str(_o, _k);
            if (!s.isEmpty()) parts.append(s);
        };
        if (_doc.contain("functions")) {
            const auto& funcs = _doc.value("functions").to<alx::json_object>();
            if (funcs.contain(_key.toStdString())) {
                for (const alx::json_value* ov : funcs.value(_key.toStdString()).to<alx::json_array>()) {
                    const auto& o = ov->to<alx::json_object>();
                    add(o, "desc");
                    if (o.contain("param"))
                        for (const alx::json_value* p : o.value("param").to<alx::json_array>()) {
                            const auto& po = p->to<alx::json_object>();
                            add(po, "key");
                            add(po, "type");
                            add(po, "desc");
                        }
                    if (o.contain("ret")) add(o.value("ret").to<alx::json_object>(), "desc");
                }
            }
        }
        if (_doc.contain("defines")) {
            const auto& defs = _doc.value("defines").to<alx::json_object>();
            if (defs.contain(_key.toStdString())) add(defs.value(_key.toStdString()).to<alx::json_object>(), "desc");
        }
        return parts.join(' ').toLower();
    }

    // First overload drives the module overview; the detail page still shows all of them.
    void summaryOf(const QString& _key, const alx::json_object& _doc, QString& _sig, QString& _desc) {
        if (_doc.contain("functions")) {
            const auto& funcs = _doc.value("functions").to<alx::json_object>();
            if (funcs.contain(_key.toStdString())) {
                const auto& ovs = funcs.value(_key.toStdString()).to<alx::json_array>();
                if (!ovs.empty()) {
                    const auto& o = ovs.front()->to<alx::json_object>();
                    _sig = signatureOf(_key, o);
                    _desc = str(o, "desc");
                }
                return;
            }
        }
        if (_doc.contain("defines")) {
            const auto& defs = _doc.value("defines").to<alx::json_object>();
            if (!defs.contain(_key.toStdString())) return;
            const auto& d = defs.value(_key.toStdString()).to<alx::json_object>();
            QString value;
            if (d.contain("value"))
                value = QString::fromStdString(alx::json_doc::to_string(d.value("value"), true));
            _sig = "$" + _key + (value.isEmpty() ? QString() : " = " + value);
            _desc = str(d, "desc");
        }
    }

} // namespace

LangRefDock::LangRefDock(QWidget* parent)
    : QWidget(parent) {
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("搜索函数 / 章节…"));
    m_search->setClearButtonEnabled(true);

    m_tree = new QTreeWidget(this);
    m_tree->setHeaderHidden(true);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setUniformRowHeights(true);
    m_tree->setMinimumWidth(140);

    m_view = new QTextBrowser(this);
    m_view->setOpenExternalLinks(true);
    m_view->setLineWrapMode(QTextEdit::WidgetWidth);
    m_view->document()->setDocumentMargin(10);

    auto* split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(m_tree);
    split->addWidget(m_view);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({180, 420});

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(4);
    lay->addWidget(m_search);
    lay->addWidget(split);
    applySettings();

    connect(m_search, &QLineEdit::textChanged, this, &LangRefDock::filterTree);
    connect(m_search, &QLineEdit::returnPressed, this, [this] {
        QTreeWidgetItem* cur = m_tree->currentItem();
        if (!cur || cur->isHidden() || cur->data(0, kKind).toInt() != KFunction) cur = nullptr;
        if (!cur)
            for (int i = 0; i < m_tree->topLevelItemCount() && !cur; ++i)
                cur = firstVisibleFunction(m_tree->topLevelItem(i));
        if (cur) {
            m_tree->setCurrentItem(cur);
            activateItem(cur);
        }
    });
    connect(m_tree, &QTreeWidget::itemClicked, this, &LangRefDock::activateItem);
    connect(m_tree, &QTreeWidget::itemActivated, this, &LangRefDock::activateItem);
}

void LangRefDock::applySettings() {
    m_view->setFont(QFont(Settings::editorFont(), Settings::editorFontSize() + 1));
}

void LangRefDock::showEvent(QShowEvent* _ev) {
    QWidget::showEvent(_ev);
    if (m_built) return;
    m_built = true;
    build();
}

void LangRefDock::build() {
    try {
        m_blocks = &core::langref();
    } catch (const std::exception& e) {
        m_view->setHtml(QStringLiteral("<p style=\"color:#fb4934\">语言参考加载失败：%1</p>")
                            .arg(esc(QString::fromUtf8(e.what()))));
        return;
    }

    m_chapters.clear();
    Chapter cur;
    bool open = false;
    for (int i = 0; i < static_cast<int>(m_blocks->size()); ++i) {
        const auto& b = (*m_blocks)[i].to<alx::json_object>();
        if (str(b, "type") != QLatin1String("heading")) continue;
        const int level = static_cast<int>(jint(b, "level", 1));
        if (level == 1) {
            if (open) {
                cur.end = i;
                m_chapters.append(cur);
            }
            cur = Chapter();
            cur.begin = i;
            cur.title = inlinePlain(str(b, "text"));
            open = true;
        } else if (open) {
            cur.sections.append({i, level, inlinePlain(str(b, "text"))});
        }
    }
    if (open) {
        cur.end = static_cast<int>(m_blocks->size());
        m_chapters.append(cur);
    }

    const auto& doc = core::document();
    m_modules.clear();
    m_tree->setUpdatesEnabled(false);
    m_tree->clear();
    m_libRoot = nullptr;

    auto* langRoot = new QTreeWidgetItem(m_tree);
    langRoot->setText(0, tr("语言"));
    langRoot->setData(0, kKind, KRoot);
    for (int i = 0; i < m_chapters.size(); ++i) {
        auto* ci = new QTreeWidgetItem(langRoot);
        ci->setText(0, m_chapters[i].title);
        ci->setToolTip(0, m_chapters[i].title);
        ci->setData(0, kKind, KChapter);
        ci->setData(0, kData, i);
        QTreeWidgetItem* h2 = nullptr;
        for (const Section& s : m_chapters[i].sections) {
            QTreeWidgetItem* parent = (s.level == 3 && h2) ? h2 : ci;
            auto* hi = new QTreeWidgetItem(parent);
            hi->setText(0, s.title);
            hi->setToolTip(0, s.title);
            hi->setData(0, kKind, KSection);
            hi->setData(0, kData, i);
            hi->setData(0, kBlock, s.block);
            if (s.level == 2) h2 = hi;
        }
    }

    auto* funcRoot = new QTreeWidgetItem(m_tree);
    funcRoot->setText(0, tr("函数手册"));
    funcRoot->setData(0, kKind, KRoot);
    buildFunctionTree(funcRoot);
    refreshLibraries();
    langRoot->setExpanded(true);
    funcRoot->setExpanded(true);
    m_tree->setUpdatesEnabled(true);

    if (!m_chapters.isEmpty()) {
        m_tree->setCurrentItem(langRoot->child(0));
        renderChapter(0);
    }
}

void LangRefDock::buildFunctionTree(QTreeWidgetItem* _root) {
    const auto& doc = core::document();
    QSet<QString> known;
    if (doc.contain("functions")) {
        const auto& funcs = doc.value("functions").to<alx::json_object>();
        for (auto it = funcs.begin(); it != funcs.end(); ++it) {
            const QString key = QString::fromStdString(it.key());
            const QString mod = key.contains('_') ? key.section('_', 0, 0) : QStringLiteral("global");
            m_modules[mod].append(key);
            known.insert(mod);
        }
    }
    if (doc.contain("defines")) {
        const auto& defs = doc.value("defines").to<alx::json_object>();
        for (auto it = defs.begin(); it != defs.end(); ++it) {
            const QString key = QString::fromStdString(it.key());
            const QString prefix = key.contains('_') ? key.section('_', 0, 0) : QString();
            const QString mod = known.contains(prefix) ? prefix : QStringLiteral("global");
            m_modules[mod].append(key);
        }
    }
    for (auto it = m_modules.begin(); it != m_modules.end(); ++it) {
        auto* mi = new QTreeWidgetItem(_root);
        mi->setText(0, QStringLiteral("%1 (%2)").arg(it.key()).arg(it.value().size()));
        mi->setData(0, kKind, KModule);
        mi->setData(0, kData, it.key());
        for (const QString& key : it.value()) {
            QString sig, desc;
            summaryOf(key, doc, sig, desc);
            m_funcHtml[key] = functionHtml(key, doc);
            m_funcText[key] = searchTextOf(key, doc);
            m_funcSig[key] = sig;
            m_funcDesc[key] = desc;
            auto* fi = new QTreeWidgetItem(mi);
            fi->setText(0, "$" + key);
            fi->setData(0, kKind, KFunction);
            fi->setData(0, kData, key);
            fi->setToolTip(0, desc.isEmpty() ? sig : desc);
        }
    }
}

void LangRefDock::renderChapter(int _index) {
    if (!m_blocks || _index < 0 || _index >= m_chapters.size()) return;
    const Chapter& ch = m_chapters[_index];
    QString html;
    for (int i = ch.begin; i < ch.end; ++i) html += blockHtml((*m_blocks)[i], i);
    m_view->setHtml(html);
    m_view->verticalScrollBar()->setValue(0);
}

void LangRefDock::scrollToBlock(int _block) {
    m_view->scrollToAnchor(QStringLiteral("b%1").arg(_block));
}

void LangRefDock::setHintProvider(HintProvider* _hints) {
    m_hints = _hints;
    if (m_built) refreshLibraries();
}

void LangRefDock::refreshLibraries() {
    if (!m_hints || !m_tree || !m_built) return; // build() rebuilds the root itself
    if (!m_libRoot) {
        m_libRoot = new QTreeWidgetItem(m_tree);
        m_libRoot->setText(0, tr("库"));
        m_libRoot->setData(0, kKind, KRoot);
    }
    const auto children = m_libRoot->takeChildren();
    for (QTreeWidgetItem* c : children) delete c;
    const QList<HintProvider::LibraryFile> files = m_hints->libraryFiles();
    QString lastDir;
    QTreeWidgetItem* dirItem = nullptr;
    for (const HintProvider::LibraryFile& f : files) {
        if (f.dir != lastDir) {
            dirItem = new QTreeWidgetItem(m_libRoot);
            dirItem->setText(0, f.dir);
            dirItem->setToolTip(0, f.dir);
            dirItem->setData(0, kKind, KRoot);
            dirItem->setExpanded(true);
            lastDir = f.dir;
        }
        auto* it = new QTreeWidgetItem(dirItem);
        it->setText(0, f.name);
        it->setToolTip(0, f.path);
        it->setData(0, kKind, f.kind == HintProvider::Kind::Link ? KLibraryLink : KLibraryImport);
        it->setData(0, kData, f.path);
    }
    if (files.isEmpty()) {
        auto* empty = new QTreeWidgetItem(m_libRoot);
        empty->setText(0, tr("（无）"));
        empty->setFlags(Qt::NoItemFlags);
    }
    m_libRoot->setExpanded(true);
}

void LangRefDock::renderLibrary(int _kind, const QString& _path) {
    if (!m_hints) return;
    const QString html =
        m_hints->documentHtml(_kind == KLibraryLink ? HintProvider::Kind::Link
                                                    : HintProvider::Kind::Import,
                              _path);
    if (html.isEmpty())
        m_view->setHtml(QStringLiteral("<p style=\"color:#fb4934\">无法读取文档：%1</p>")
                            .arg(esc(_path)));
    else
        m_view->setHtml(html);
    m_view->verticalScrollBar()->setValue(0);
}

void LangRefDock::renderFunction(const QString& _key) {
    m_view->setHtml(m_funcHtml.value(_key));
    m_view->verticalScrollBar()->setValue(0);
}

void LangRefDock::renderModule(const QString& _module) {
    const QStringList keys = m_modules.value(_module);
    QString html = "<h2 style=\"color:#fbf1c7\">" + esc(_module) + "</h2>";
    html += QStringLiteral("<p style=\"color:#928374\">%1 functions / constants</p>").arg(keys.size());
    for (const QString& key : keys) {
        html += "<p><span style=\"color:#83a598;font-family:'Fira Code'\">" +
                esc(m_funcSig.value(key)) + "</span>";
        const QString desc = m_funcDesc.value(key);
        if (!desc.isEmpty()) html += "<br><span style=\"color:#a89984\">" + esc(desc) + "</span>";
        html += "</p>";
    }
    m_view->setHtml(html);
    m_view->verticalScrollBar()->setValue(0);
}

void LangRefDock::filterTree(const QString& _text) {
    if (!m_built) return;
    QString q = _text.trimmed();
    if (q.startsWith('$')) q.remove(0, 1);

    if (q.isEmpty()) { // reset: roots expanded, everything else collapsed
        std::function<void(QTreeWidgetItem*, bool)> reset = [&](QTreeWidgetItem* _item, bool _top) {
            _item->setHidden(false);
            _item->setExpanded(_top);
            for (int i = 0; i < _item->childCount(); ++i)
                reset(_item->child(i), false);
        };
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
            reset(m_tree->topLevelItem(i), true);
        return;
    }

    std::function<void(QTreeWidgetItem*)> showSubtree = [&](QTreeWidgetItem* _item) {
        _item->setHidden(false);
        for (int i = 0; i < _item->childCount(); ++i)
            showSubtree(_item->child(i));
    };
    std::function<bool(QTreeWidgetItem*)> filter = [&](QTreeWidgetItem* _item) {
        const int kind = _item->data(0, kKind).toInt();
        const bool self = kind == KFunction
                              ? m_funcText.value(_item->data(0, kData).toString())
                                    .contains(q, Qt::CaseInsensitive)
                              : _item->text(0).contains(q, Qt::CaseInsensitive);
        if (self) {
            showSubtree(_item);
            _item->setExpanded(kind != KFunction);
            return true;
        }
        bool childAny = false;
        for (int i = 0; i < _item->childCount(); ++i) childAny |= filter(_item->child(i));
        _item->setHidden(!childAny);
        if (childAny) _item->setExpanded(true);
        return childAny;
    };
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) filter(m_tree->topLevelItem(i));
}

QTreeWidgetItem* LangRefDock::firstVisibleFunction(QTreeWidgetItem* _item) const {
    if (!_item || _item->isHidden()) return nullptr;
    if (_item->data(0, kKind).toInt() == KFunction) return _item;
    for (int i = 0; i < _item->childCount(); ++i)
        if (QTreeWidgetItem* f = firstVisibleFunction(_item->child(i))) return f;
    return nullptr;
}

void LangRefDock::activateItem(QTreeWidgetItem* _item) {
    if (!_item || _item->isHidden()) return;
    switch (_item->data(0, kKind).toInt()) {
    case KChapter: renderChapter(_item->data(0, kData).toInt()); break;
    case KSection:
        renderChapter(_item->data(0, kData).toInt());
        scrollToBlock(_item->data(0, kBlock).toInt());
        break;
    case KFunction: renderFunction(_item->data(0, kData).toString()); break;
    case KModule: renderModule(_item->data(0, kData).toString()); break;
    case KLibraryLink:
    case KLibraryImport: renderLibrary(_item->data(0, kKind).toInt(), _item->data(0, kData).toString()); break;
    case KRoot: _item->setExpanded(!_item->isExpanded()); break;
    default: break;
    }
}
