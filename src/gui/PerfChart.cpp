#include "PerfChart.h"

#include "QtCompat.h"

#include <QFont>
#include <QPainter>
#include <QPalette>
#include <QPolygonF>

#include <algorithm>
#include <utility>

PerfChart::PerfChart(const QString& _title, bool _percent, QColor _light, QColor _dark,
                     QWidget* _parent)
    : QWidget(_parent), m_title(_title), m_percent(_percent), m_light(std::move(_light)),
      m_dark(std::move(_dark)) {
    setMinimumHeight(100);
}

void PerfChart::reset() {
    m_head = 0;
    m_count = 0;
    m_peak = 0;
    update();
}

void PerfChart::add(double _value) {
    if (_value < 0) _value = 0;
    m_data[m_head] = _value;
    m_head = (m_head + 1) % kCapacity;
    if (m_count < kCapacity) ++m_count;
    m_peak = 0;
    const int used = m_count < kCapacity ? m_head : kCapacity;
    for (int i = 0; i < used; ++i)
        if (m_data[i] > m_peak) m_peak = m_data[i];
    update();
}

void PerfChart::setLimit(double _limit) {
    m_limit = _limit > 0 ? _limit : 0;
    update();
}

void PerfChart::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRect box = rect().adjusted(0, 0, -1, -1);
    p.setPen(QPen(palette().color(QPalette::Mid), 1));
    p.setBrush(palette().color(QPalette::Base));
    p.drawRoundedRect(box, 3, 3);

    QFont f = font();
    f.setBold(true);
    p.setFont(f);
    p.setPen(palette().color(QPalette::Text));
    p.drawText(box.adjusted(8, 4, -8, 0), Qt::AlignLeft | Qt::AlignTop, m_title);

    f.setBold(false);
    f.setPointSizeF(std::max(7.0, f.pointSizeF() - 1));
    p.setFont(f);
    p.setPen(axc_placeholder_color(palette()));
    const QString meta =
        m_count ? QStringLiteral("当前 %1 · 峰值 %2")
                      .arg(valueText(m_data[(m_head + kCapacity - 1) % kCapacity]), valueText(m_peak))
                : QStringLiteral("无数据");
    p.drawText(box.adjusted(8, 4, -8, 0), Qt::AlignRight | Qt::AlignTop, meta);

    const QRect plot = box.adjusted(8, 24, -8, -8);
    if (plot.width() < 12 || plot.height() < 12) return;

    const double top = yMax();
    const auto y_of = [&](double _v) {
        return plot.bottom() - plot.height() * std::clamp(_v / top, 0.0, 1.0);
    };

    p.setPen(QPen(palette().color(QPalette::Mid), 1, Qt::DotLine));
    for (int i = 0; i <= 2; ++i) {
        const int y = plot.top() + plot.height() * i / 2;
        p.drawLine(plot.left(), y, plot.right(), y);
    }
    p.setPen(axc_placeholder_color(palette()));
    p.drawText(plot.adjusted(2, 1, -2, 0), Qt::AlignRight | Qt::AlignTop, valueText(top));

    if (m_limit > 0) {
        const int y = static_cast<int>(y_of(m_limit));
        p.setPen(QPen(axc_placeholder_color(palette()), 1, Qt::DashLine));
        p.drawLine(plot.left(), y, plot.right(), y);
        p.drawText(plot.adjusted(2, 2, -2, 0), Qt::AlignLeft | Qt::AlignTop,
                   QStringLiteral("上限 %1").arg(valueText(m_limit)));
    }

    if (m_count == 0) return;

    QPolygonF line;
    line.reserve(m_count);
    for (int i = 0; i < m_count; ++i) {
        const int slot = (m_head - m_count + i + kCapacity) % kCapacity;
        const double x = plot.left() + plot.width() * static_cast<double>(i) / (kCapacity - 1);
        line << QPointF(x, y_of(m_data[slot]));
    }
    p.setPen(QPen(lineColor(), 2));
    p.setBrush(Qt::NoBrush);
    p.drawPolyline(line);

    const QPointF last = line.last();
    p.setPen(QPen(palette().color(QPalette::Base), 2));
    p.setBrush(lineColor());
    p.drawEllipse(last, 4, 4);
}

QString PerfChart::valueText(double _value) const {
    if (m_percent) return QString::number(_value, 'f', 1) + QLatin1Char('%');
    if (_value >= 10 * 1024 * 1024)
        return QString::number(_value / (1024.0 * 1024.0), 'f', 1) + QStringLiteral(" MiB");
    return QString::number(_value / 1024.0, 'f', 0) + QStringLiteral(" KiB");
}

QColor PerfChart::lineColor() const {
    return palette().color(QPalette::Window).lightness() < 128 ? m_dark : m_light;
}

double PerfChart::yMax() const {
    double top = std::max(m_peak, m_limit);
    if (m_percent) return std::max(10.0, top * 1.15);
    return std::max(1024.0 * 1024.0, top * 1.15);
}
