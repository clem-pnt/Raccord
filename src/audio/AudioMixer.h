#pragma once
// Renders the audio of a sequence: clips -> tracks -> master.
#include "AudioEffects.h"
#include "core/Model.h"
#include <map>
#include <memory>
#include <vector>

namespace rc {

struct MeterLevels {
    std::vector<float> peak;   // per channel, linear
    std::vector<float> rms;
};

class AudioMixer {
public:
    AudioMixer() = default;
    // Mixes `frames` samples starting at sample `pos` (at seq.sampleRate) into `out`
    // (interleaved, seq.audioChannels). Keeps DSP state for contiguous calls.
    void mix(const Project &project, const Sequence &seq, int64_t pos, int frames, float *out);
    void reset();
    // Levels of the last mixed block (master and per track) for meters.
    const MeterLevels &masterLevels() const { return m_master; }
    const std::vector<MeterLevels> &trackLevels() const { return m_tracks; }
    // Ignore solo / mute (used for analysis of single tracks).
    void setSoloTrack(int t) { m_forceTrack = t; }
    // Mixes a single media item (source monitor playback).
    static void mixMedia(const MediaItem &item, int rate, int channels, int64_t pos, int frames, float *out);

private:
    void mixTrack(const Project &project, const Sequence &seq, const Track &track, int ti, int64_t pos, int frames, float *out, int depth);
    void renderClip(const Project &project, const Sequence &seq, const Track &track, const Clip &clip, int64_t pos, int frames,
                    float *out, float gainScale, int depth);
    void runChain(const QString &owner, const std::vector<EffectInstance> &chain, float *buf, int frames, int channels, int rate,
                  Ticks localStart, const float *sidechain = nullptr);

    std::map<QString, std::unique_ptr<AudioProcessor>> m_procs;   // key: owner id + effect uid
    std::map<QString, std::vector<float>> m_trackBuffers;         // last mixed block per track (sidechain)
    int64_t m_nextPos = -1;
    MeterLevels m_master;
    std::vector<MeterLevels> m_tracks;
    int m_forceTrack = -1;
    std::unique_ptr<AudioMixer> m_nested;
};

// Constant power / gain fade curves for audio transitions.
float crossfadeGain(const QString &type, double progress, bool fadingIn);

} // namespace rc
