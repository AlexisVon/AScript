#include "CompletionPopup.h"
#include "QtCompat.h"

#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>

namespace {

    // gruvbox palette — matches AsHighlighter / ScriptEditor
    const QColor kBg(0x28, 0x28, 0x28);
    const QColor kSel(0x50, 0x49, 0x45);
    const QColor kDim(0xa8, 0x99, 0x84);

    QColor kindColor(CompletionItemData::Kind _kind) {
        switch (_kind) {
        case CompletionItemData::Extend: return QColor(0x8e, 0xc0, 0x7c); // aqua
        case CompletionItemData::Define:
        case CompletionItemData::Literal: return QColor(0xfe, 0x80, 0x19); // orange
        case CompletionItemData::Keyword: return QColor(0xfb, 0x49, 0x34); // red
        case CompletionItemData::Builtin: return QColor(0xd3, 0x86, 0x9b); // purple
        case CompletionItemData::Link: return QColor(0x8e, 0xc0, 0x7c); // green
        case CompletionItemData::Import: return QColor(0x83, 0xa5, 0x98); // blue
        default: return QColor(0xeb, 0xdb, 0xb2); // fg1 — variables
        }
    }

    // source tag: link/import/base take distinct colors
    QColor sourceColor(CompletionItemData::Kind _kind) {
        switch (_kind) {
        case CompletionItemData::Link: return QColor(0x8e, 0xc0, 0x7c);
        case CompletionItemData::Import: return QColor(0x83, 0xa5, 0x98);
        default: return QColor(0xa8, 0x99, 0x84);
        }
    }

} // namespace

// Two-line item painter: line 1 = kind-colored bold name + dim sig, line 2 =
// dim desc; items without a desc collapse to a single row (no empty gap)
class CompletionPopup::Delegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& _opt, const QModelIndex& _idx) const override {
        const auto d = _idx.data(Qt::UserRole).value<CompletionItemData>();
        const int fmh = _opt.fontMetrics.height();
        const int nameW = AXC_FONT_METRICS_TEXT_WIDTH(_opt.fontMetrics, d.name);
        const int sigW = d.sig.isEmpty() ? 0 : AXC_FONT_METRICS_TEXT_WIDTH(_opt.fontMetrics, d.sig) + 8;
        const int srcW = d.source.isEmpty() ? 0 : AXC_FONT_METRICS_TEXT_WIDTH(_opt.fontMetrics, d.source) + 12;
        const int descW = d.desc.isEmpty() ? 0 : AXC_FONT_METRICS_TEXT_WIDTH(_opt.fontMetrics, d.desc);
        const int w = qMax(nameW + sigW + srcW, descW) + 24; // 8+8 padding + 8 margin
        return {w, (d.desc.isEmpty() ? fmh : fmh * 2) + 8};
    }

    void paint(QPainter* _p, const QStyleOptionViewItem& _opt, const QModelIndex& _idx) const override {
        const auto d = _idx.data(Qt::UserRole).value<CompletionItemData>();
        _p->fillRect(_opt.rect, (_opt.state & QStyle::State_Selected) ? kSel : kBg);
        const int fmh = _opt.fontMetrics.height();
        const int twoLine = !d.desc.isEmpty();
        QRect line1 = _opt.rect.adjusted(8, 3, -8, twoLine ? -(fmh + 5) : -3);
        _p->save();
        _p->setClipRect(line1); // sig must not bleed into the desc row below
        QFont f = _opt.font;
        f.setBold(true);
        _p->setFont(f);
        _p->setPen(kindColor(d.kind));
        const int nameW = AXC_FONT_METRICS_TEXT_WIDTH(_p->fontMetrics(), d.name);
        _p->drawText(line1, Qt::AlignLeft | Qt::AlignVCenter, d.name);
        _p->setFont(_opt.font);
        int srcW = 0;
        if (!d.source.isEmpty()) {
            srcW = AXC_FONT_METRICS_TEXT_WIDTH(_opt.fontMetrics, d.source) + 12;
            _p->setPen(sourceColor(d.kind));
            _p->drawText(line1, Qt::AlignRight | Qt::AlignVCenter, d.source);
        }
        _p->setPen(kDim);
        const int sigW = line1.width() - nameW - 8 - srcW;
        if (!d.sig.isEmpty() && sigW > 0)
            _p->drawText(QRect(line1.left() + nameW + 8, line1.top(), sigW, line1.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, d.sig);
        _p->restore();
        if (twoLine)
            _p->drawText(_opt.rect.adjusted(8, fmh + 5, -8, -3), Qt::AlignLeft | Qt::AlignVCenter, d.desc);
    }
};

CompletionPopup::CompletionPopup(QWidget* _parent)
    : QListWidget(_parent) {
    setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);
    setFocusPolicy(Qt::NoFocus); // keyboard stays on the editor (IME-safe)
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setItemDelegate(new Delegate(this)); // per-item height: desc present → two rows
    setStyleSheet(QStringLiteral(
        "QListWidget { background: #282828; border: 1px solid #504945; }"
        "QListWidget::item { padding: 0px; }"
        "QListWidget::item:selected { background: #504945; }"));
    hide();
}

void CompletionPopup::showAt(const QPoint& _globalPos, const QList<CompletionItemData>& _items,
                             int _windowRows) {
    clear();
    const QFontMetrics fm(fontMetrics());
    int maxW = 0;
    for (const auto& d : _items) {
        auto* it = new QListWidgetItem;
        it->setData(Qt::UserRole, QVariant::fromValue(d));
        addItem(it);
        const int nameW = AXC_FONT_METRICS_TEXT_WIDTH(fm, d.name);
        const int sigW = d.sig.isEmpty() ? 0 : AXC_FONT_METRICS_TEXT_WIDTH(fm, d.sig) + 8;
        const int srcW = d.source.isEmpty() ? 0 : AXC_FONT_METRICS_TEXT_WIDTH(fm, d.source) + 12;
        const int descW = d.desc.isEmpty() ? 0 : AXC_FONT_METRICS_TEXT_WIDTH(fm, d.desc);
        maxW = qMax(maxW, qMax(nameW + sigW + srcW, descW));
    }
    const int fmh = fm.height();
    const int rows = qMin(_items.size(), _windowRows);
    int h = 0;
    for (int i = 0; i < rows; ++i) h += _items[i].desc.isEmpty() ? fmh + 8 : fmh * 2 + 8;
    const int w = qBound(200, maxW + 24, 800); // 8+8 padding + 8 margin, clamped
    resize(w, h + 2);
    QScreen* scr = AXC_GUI_APPLICATION_SCREEN_AT(_globalPos);
    const QRect avail = scr ? scr->availableGeometry()
                            : QGuiApplication::primaryScreen()->availableGeometry();
    QPoint p = _globalPos + QPoint(0, 4);
    if (p.x() + width() > avail.right()) p.setX(avail.right() - width());
    if (p.y() + height() > avail.bottom()) p.setY(_globalPos.y() - height() - 4); // flip above
    move(p);
    setCurrentRow(0);
    show();
}

CompletionItemData CompletionPopup::currentData() const {
    if (QListWidgetItem* it = currentItem()) return it->data(Qt::UserRole).value<CompletionItemData>();
    return {};
}

void CompletionPopup::mouseReleaseEvent(QMouseEvent* _ev) {
    if (itemAt(_ev->pos())) emit insertRequested(); // click inserts — the editor replaces the word
}
