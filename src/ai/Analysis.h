#pragma once
// Media analysis used by the "assistant" features: scene cuts, silences,
// automatic reframing, stabilisation and speech transcription.
// All functions are blocking and meant to run on a worker thread.
#include "core/Model.h"
#include <functional>
#include <vector>

namespace rc::ai {

using Progress = std::function<bool(double fraction)>;   // return false to cancel

// Decodes video frames between in and out (source ticks) scaled to w x h.
// gray: 1 byte per pixel, otherwise RGB24. cb(time, data, stride) -> false stops.
bool scanFrames(const QString &path, Ticks in, Ticks out, int w, int h, bool gray,
                const std::function<bool(Ticks, const uint8_t *, int)> &cb, QString *error = nullptr);

// Returns source times of detected shot changes in [in, out).
std::vector<Ticks> detectScenes(const QString &path, Ticks in, Ticks out, double sensitivity /*0..1*/,
                                const Progress &progress = {}, QString *error = nullptr);

// Silent ranges (source ticks) in [in, out): level below thresholdDb for at
// least minDuration. Ranges are shrunk by padding on both sides.
std::vector<TimeRange> detectSilence(const QString &path, int audioStream, Ticks in, Ticks out, double thresholdDb,
                                     Ticks minDuration, Ticks padding, const Progress &progress = {}, QString *error = nullptr);

// Subject tracking for auto reframe: (source time, horizontal and vertical
// centre in 0..1) samples, smoothed.
struct ReframeSample { Ticks time; double x; double y; };
std::vector<ReframeSample> reframeTrack(const QString &path, Ticks in, Ticks out, double smoothing /*0..1*/,
                                        const Progress &progress = {}, QString *error = nullptr);

// Stabilisation: per output frame [dx, dy, rotation, scale] corrections in uv
// units (format expected by the Warp Stabilizer effect), sampled at `fps`.
QString analyzeStabilization(const QString &path, Ticks in, double speed, Ticks duration, FrameRate fps, double smoothness /*0..100*/,
                             bool rotation, const Progress &progress = {}, QString *error = nullptr);

// Speech to text.
struct Word { Ticks start = 0, end = 0; QString text; };
struct Segment { Ticks start = 0, end = 0; QString text; std::vector<Word> words; };
// Where the transcription engine lives (a Python venv with faster-whisper).
QString whisperHome();
bool whisperAvailable(QString *detail = nullptr);
// Installs faster-whisper into whisperHome() (needs python3 and network).
bool installWhisper(const std::function<void(const QString &)> &log, QString *error);
// Transcribes the audio of a media file between in and out. model: tiny, base,
// small, medium, large-v3. language: ISO code or "auto".
std::vector<Segment> transcribe(const QString &path, int audioStream, Ticks in, Ticks out, const QString &model,
                                const QString &language, const Progress &progress = {}, QString *error = nullptr);
// Splits segments into readable captions (max chars per line, lines, duration).
std::vector<Caption> segmentsToCaptions(const std::vector<Segment> &segs, int maxChars, int maxLines, Ticks maxDuration);

} // namespace rc::ai
