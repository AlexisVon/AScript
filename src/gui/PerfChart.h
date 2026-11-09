#ifndef PERFCHART_H
#define PERFCHART_H

#include <QColor>
#include <QString>
#include <QWidget>

#include <array>

// PerfChart — single-series auto-scaled line chart over a fixed sample ring
class PerfChart : public QWidget {
    Q_OBJECT

public:
    PerfChart(const QString& _title, bool _percent, QColor _light, QColor _dark,
              QWidget* _parent = nullptr);

    void reset();
    void add(double _value);
    void setLimit(double _limit); // same unit as add(); 0 = no limit line

protected:
    void paintEvent(QPaintEvent* _event) override;

private:
    QString valueText(double _value) const;
    QColor lineColor() const;
    double yMax() const;

    static constexpr int kCapacity = 300;

    QString m_title;
    bool m_percent;
    QColor m_light;
    QColor m_dark;
    double m_limit = 0;
    std::array<double, kCapacity> m_data{};
    int m_head = 0; // next write slot
    int m_count = 0;
    double m_peak = 0;
};

#endif // PERFCHART_H
