#include "Time.h"
#include <QStringList>
#include <QRegularExpression>

namespace rc {

FrameRate FrameRate::fromDouble(double fps)
{
    struct K { double f; int n, d; };
    static const K known[] = {
        {23.976, 24000, 1001}, {24, 24, 1}, {25, 25, 1}, {29.97, 30000, 1001}, {30, 30, 1},
        {47.952, 48000, 1001}, {48, 48, 1}, {50, 50, 1}, {59.94, 60000, 1001}, {60, 60, 1},
        {90, 90, 1}, {100, 100, 1}, {119.88, 120000, 1001}, {120, 120, 1}, {15, 15, 1},
        {12, 12, 1}, {10, 10, 1}, {8, 8, 1}, {6, 6, 1}, {240, 240, 1},
    };
    for (const auto &k : known)
        if (std::abs(fps - k.f) < 0.01)
            return {k.n, k.d};
    if (fps <= 0)
        return {25, 1};
    // Fallback: round to 1/1000 fps.
    return {(int)std::lround(fps * 1000), 1000};
}

QString FrameRate::toString() const
{
    if (den == 1)
        return QString::number(num);
    return QString::number(fps(), 'f', 3).remove(QRegularExpression("0+$")).remove(QRegularExpression("\\.$"));
}

static QString two(int64_t v) { return QString::number(v).rightJustified(2, '0'); }

QString formatTimecode(Ticks t, FrameRate r, bool dropFrame)
{
    bool neg = t < 0;
    if (neg)
        t = -t;
    int64_t frame = ticksToFrame(t, r);
    const int fpsN = std::max(1, r.nominal());
    const bool df = dropFrame && r.isNtsc() && (fpsN == 30 || fpsN == 60);
    if (df) {
        // SMPTE drop frame: skip frame numbers 0,1 (or 0-3 at 60) every minute except every 10th.
        const int64_t drop = fpsN == 60 ? 4 : 2;
        const int64_t framesPer10Min = fpsN * 600 - drop * 9;
        const int64_t framesPerMin = fpsN * 60 - drop;
        int64_t d = frame / framesPer10Min;
        int64_t m = frame % framesPer10Min;
        if (m > drop)
            frame += drop * 9 * d + drop * ((m - drop) / framesPerMin);
        else
            frame += drop * 9 * d;
    }
    int64_t ff = frame % fpsN;
    int64_t totalSec = frame / fpsN;
    int64_t ss = totalSec % 60;
    int64_t mm = (totalSec / 60) % 60;
    int64_t hh = totalSec / 3600;
    QString s = two(hh) + ":" + two(mm) + ":" + two(ss) + (df ? ";" : ":") + two(ff);
    return neg ? "-" + s : s;
}

QString formatTime(Ticks t, FrameRate r, TimeDisplay mode)
{
    switch (mode) {
    case TimeDisplay::Frames:
        return QString::number(ticksToFrame(t, r));
    case TimeDisplay::Seconds:
        return QString::number(ticksToSeconds(t), 'f', 3) + " s";
    case TimeDisplay::Feet: {
        // 35mm: 16 frames per foot.
        int64_t f = ticksToFrame(t, r);
        return QString::number(f / 16) + "+" + two(f % 16);
    }
    case TimeDisplay::Timecode:
    default:
        return formatTimecode(t, r, true);
    }
}

Ticks parseTimecode(const QString &input, FrameRate r, Ticks relativeTo)
{
    QString s = input.trimmed();
    if (s.isEmpty())
        return kInvalidTicks;
    int sign = 0;
    if (s.startsWith('+')) { sign = 1; s.remove(0, 1); }
    else if (s.startsWith('-')) { sign = -1; s.remove(0, 1); }
    const int fpsN = std::max(1, r.nominal());
    int64_t frames = 0;
    if (s.contains(':') || s.contains(';') || s.contains('.')) {
        QStringList parts = s.split(QRegularExpression("[:;.]"));
        if (parts.size() > 4)
            return kInvalidTicks;
        int64_t vals[4] = {0, 0, 0, 0};
        int off = 4 - parts.size();
        for (int i = 0; i < parts.size(); ++i) {
            bool ok = false;
            vals[off + i] = parts[i].isEmpty() ? 0 : parts[i].toLongLong(&ok);
            if (!parts[i].isEmpty() && !ok)
                return kInvalidTicks;
        }
        frames = ((vals[0] * 3600 + vals[1] * 60 + vals[2]) * fpsN) + vals[3];
        const bool df = s.contains(';') && r.isNtsc() && (fpsN == 30 || fpsN == 60);
        if (df) {
            const int64_t drop = fpsN == 60 ? 4 : 2;
            const int64_t totalMinutes = vals[0] * 60 + vals[1];
            frames -= drop * (totalMinutes - totalMinutes / 10);
        }
    } else {
        // Premiere convention: plain digits are read as a compact timecode
        // (e.g. "1000" = 00:00:10:00) unless it is a pure frame offset.
        bool ok = false;
        int64_t v = s.toLongLong(&ok);
        if (!ok)
            return kInvalidTicks;
        if (sign != 0) {
            frames = v;
        } else {
            QString padded = s.rightJustified(8, '0');
            int64_t hh = padded.mid(0, 2).toLongLong();
            int64_t mm = padded.mid(2, 2).toLongLong();
            int64_t ss = padded.mid(4, 2).toLongLong();
            int64_t ff = padded.mid(6, 2).toLongLong();
            frames = ((hh * 3600 + mm * 60 + ss) * fpsN) + ff;
        }
    }
    Ticks t = frameToTicks(frames, r);
    if (sign > 0)
        return relativeTo + t;
    if (sign < 0)
        return relativeTo - t;
    return t;
}

} // namespace rc
