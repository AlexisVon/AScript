#include "PerfSampler.h"

#include <QDateTime>
#include <QFile>

#ifdef _WIN32
#    include <windows.h>
#    include <psapi.h>
#else
#    include <unistd.h>
#endif

namespace {
#ifdef _WIN32
    constexpr double kTicksPerSecond = 1e7; // FILETIME unit is 100 ns
#else
    double ticks_per_second() {
        static const double hz = static_cast<double>(sysconf(_SC_CLK_TCK));
        return hz > 0 ? hz : 100.0;
    }
#endif
} // namespace

PerfSampler::PerfSampler(QObject* _parent)
    : QObject(_parent) {
    m_timer.setInterval(500);
    connect(&m_timer, &QTimer::timeout, this, &PerfSampler::sample);
}

void PerfSampler::start(quint64 _pid) {
    reset();
    if (!_pid) return;
    double cpu = 0;
    quint64 rss = 0;
    if (!read_process(_pid, cpu, rss)) return;
    m_pid = _pid;
    m_lastCpu = cpu;
    m_lastMs = QDateTime::currentMSecsSinceEpoch();
    emit sampled(0.0, rss); // short runs still get one point
    m_timer.start();
}

void PerfSampler::stop() {
    m_timer.stop();
    m_pid = 0;
    m_lastCpu = 0;
    m_lastMs = 0;
}

void PerfSampler::reset() {
    stop();
}

void PerfSampler::sample() {
    double cpu = 0;
    quint64 rss = 0;
    if (!read_process(m_pid, cpu, rss)) {
        stop();
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 dt = now - m_lastMs;
    if (dt <= 0) return;
    const double pct = (cpu - m_lastCpu) * 1000.0 / static_cast<double>(dt) * 100.0;
    m_lastCpu = cpu;
    m_lastMs = now;
    emit sampled(pct < 0 ? 0.0 : pct, rss);
}

bool PerfSampler::read_process(quint64 _pid, double& _cpuSeconds, quint64& _rssBytes) const {
    if (!_pid) return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(_pid));
    if (!h) return false;
    FILETIME created{}, exited{}, kernel{}, user{};
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    const bool ok = GetProcessTimes(h, &created, &exited, &kernel, &user) &&
                    GetProcessMemoryInfo(h, &pmc, sizeof(pmc));
    CloseHandle(h);
    if (!ok) return false;
    const auto ticks = [](const FILETIME& _t) {
        return (static_cast<quint64>(_t.dwHighDateTime) << 32) | _t.dwLowDateTime;
    };
    _cpuSeconds = static_cast<double>(ticks(kernel) + ticks(user)) / kTicksPerSecond;
    _rssBytes = static_cast<quint64>(pmc.WorkingSetSize);
    return true;
#else
    QFile stat(QStringLiteral("/proc/%1/stat").arg(_pid));
    if (!stat.open(QIODevice::ReadOnly)) return false;
    const QByteArray data = stat.readAll();
    const int close = data.lastIndexOf(')');
    if (close < 0) return false;
    // after "comm)" field 3 is state; utime/stime are fields 14/15 → indexes 11/12
    const QList<QByteArray> fields = data.mid(close + 2).split(' ');
    if (fields.size() < 13) return false;
    bool ok_u = false;
    bool ok_s = false;
    const quint64 utime = fields[11].toULongLong(&ok_u);
    const quint64 stime = fields[12].toULongLong(&ok_s);
    if (!ok_u || !ok_s) return false;
    _cpuSeconds = static_cast<double>(utime + stime) / ticks_per_second();

    QFile status(QStringLiteral("/proc/%1/status").arg(_pid));
    if (!status.open(QIODevice::ReadOnly)) return false;
    // readAll: /proc files report size 0, so atEnd()/readLine loops stop immediately
    const QList<QByteArray> lines = status.readAll().split('\n');
    for (const QByteArray& line : lines) {
        if (!line.startsWith("VmRSS:")) continue;
        const QList<QByteArray> parts = line.mid(6).trimmed().split(' ');
        bool ok = false;
        const quint64 kb = parts.isEmpty() ? 0 : parts.first().toULongLong(&ok);
        if (!ok) return false;
        _rssBytes = kb * 1024;
        return true;
    }
    return false;
#endif
}
