#pragma once
// Real-time safe audio DSP used by clip, track and master effect chains.
#include "core/Effects.h"
#include <memory>
#include <vector>

namespace rc {

struct AudioBlockInfo {
    int sampleRate = 48000;
    int channels = 2;
    Ticks localStart = 0;      // time of the first sample relative to the clip/track start
    const float *sidechain = nullptr;   // optional interleaved sidechain (same layout)
};

class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;
    virtual void process(float *buf, int frames, const EffectInstance &e, const AudioBlockInfo &info) = 0;
    virtual void reset() {}
};

std::unique_ptr<AudioProcessor> createAudioProcessor(const QString &type);

// Simple biquad (RBJ cookbook).
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1[8] = {0}, z2[8] = {0};
    void lowShelf(double fs, double f, double gainDb, double q = 0.707);
    void highShelf(double fs, double f, double gainDb, double q = 0.707);
    void peak(double fs, double f, double gainDb, double q);
    void lowpass(double fs, double f, double q = 0.707);
    void highpass(double fs, double f, double q = 0.707);
    void bandpass(double fs, double f, double q);
    inline float run(float x, int ch)
    {
        double y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return float(y);
    }
    void reset() { for (int i = 0; i < 8; ++i) z1[i] = z2[i] = 0; }
};

inline float dbToGain(double db) { return db <= -96 ? 0.f : float(std::pow(10.0, db / 20.0)); }
inline double gainToDb(double g) { return g <= 1e-6 ? -120.0 : 20.0 * std::log10(g); }

} // namespace rc
