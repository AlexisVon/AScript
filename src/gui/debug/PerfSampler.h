#ifndef AGUI_PERFSAMPLER_H
#define AGUI_PERFSAMPLER_H

#include <QObject>
#include <QTimer>

// PerfSampler — polls one process every 500 ms; stops itself when the pid is gone
class PerfSampler : public QObject {
    Q_OBJECT

public:
    explicit PerfSampler(QObject* _parent = nullptr);

    void start(quint64 _pid);
    void stop();
    void reset();
    bool active() const { return m_pid != 0; }

signals:
    void sampled(double _cpuPercent, quint64 _rssBytes);

private:
    void sample();
    bool read_process(quint64 _pid, double& _cpuSeconds, quint64& _rssBytes) const;

    QTimer m_timer;
    quint64 m_pid = 0;
    double m_lastCpu = 0;
    qint64 m_lastMs = 0;
};

#endif // AGUI_PERFSAMPLER_H
