#pragma once
// Time representation for Raccord.
// All timeline positions are integer "ticks". 254016000000 ticks per second is
// divisible by every common frame rate (23.976, 24, 25, 29.97, 30, 48, 50,
// 59.94, 60, 120) and audio sample rate (44100, 48000, 96000), so frame and
// sample boundaries are exact integers: no floating point drift on long edits.
#include <cstdint>
#include <cmath>
#include <QString>

namespace rc {

using Ticks = int64_t;
constexpr Ticks kTicksPerSecond = 254016000000LL;
constexpr Ticks kInvalidTicks = INT64_MIN;

struct FrameRate {
    int num = 25;
    int den = 1;

    constexpr bool operator==(const FrameRate &o) const { return (int64_t)num * o.den == (int64_t)o.num * den; }
    constexpr bool operator!=(const FrameRate &o) const { return !(*this == o); }
    constexpr double fps() const { return den ? double(num) / den : 0.0; }
    constexpr bool valid() const { return num > 0 && den > 0; }
    // Duration of one frame in ticks (exact for all standard rates).
    constexpr Ticks frameTicks() const { return valid() ? kTicksPerSecond * den / num : kTicksPerSecond / 25; }
    // NTSC style rates (x000/1001) that support drop frame timecode.
    constexpr bool isNtsc() const { return den == 1001; }
    int nominal() const { return (int)std::lround(fps()); }

    static FrameRate fromDouble(double fps);
    QString toString() const;
};

inline double ticksToSeconds(Ticks t) { return double(t) / double(kTicksPerSecond); }
inline Ticks secondsToTicks(double s) { return (Ticks)std::llround(s * double(kTicksPerSecond)); }

// Frame index containing tick t (floor).
inline int64_t ticksToFrame(Ticks t, FrameRate r)
{
    const Ticks ft = r.frameTicks();
    return t >= 0 ? t / ft : -((-t + ft - 1) / ft);
}
inline Ticks frameToTicks(int64_t f, FrameRate r) { return f * r.frameTicks(); }
// Snap a tick value to the nearest frame boundary.
inline Ticks snapToFrame(Ticks t, FrameRate r)
{
    const Ticks ft = r.frameTicks();
    return frameToTicks(ticksToFrame(t + ft / 2, r), r);
}
inline Ticks floorToFrame(Ticks t, FrameRate r) { return frameToTicks(ticksToFrame(t, r), r); }

inline int64_t ticksToSamples(Ticks t, int sampleRate) { return t / (kTicksPerSecond / sampleRate); }
inline Ticks samplesToTicks(int64_t s, int sampleRate) { return s * (kTicksPerSecond / sampleRate); }

// Rational from FFmpeg time base -> ticks.
inline Ticks rationalToTicks(int64_t value, int num, int den)
{
    // value * num / den seconds, computed with 128 bit intermediate to avoid overflow.
    __int128 v = (__int128)value * num * kTicksPerSecond;
    return (Ticks)(v / den);
}
inline int64_t ticksToRational(Ticks t, int num, int den)
{
    __int128 v = (__int128)t * den;
    return (int64_t)(v / ((__int128)num * kTicksPerSecond));
}

enum class TimeDisplay { Timecode, Frames, Seconds, Feet };

// SMPTE timecode, e.g. 01:02:03:04 (non drop) or 01:02:03;04 (drop frame).
QString formatTimecode(Ticks t, FrameRate r, bool dropFrame = true);
QString formatTime(Ticks t, FrameRate r, TimeDisplay mode);
// Parses "hh:mm:ss:ff", "mm:ss:ff", "ss:ff", "+10" (frames offset) or plain frames.
// Returns kInvalidTicks on failure. relativeTo used for +/- offsets.
Ticks parseTimecode(const QString &s, FrameRate r, Ticks relativeTo = 0);

struct TimeRange {
    Ticks start = 0;
    Ticks end = 0;
    Ticks duration() const { return end - start; }
    bool contains(Ticks t) const { return t >= start && t < end; }
    bool overlaps(const TimeRange &o) const { return start < o.end && o.start < end; }
    bool empty() const { return end <= start; }
};

} // namespace rc
