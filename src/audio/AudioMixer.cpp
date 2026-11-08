#include "AudioMixer.h"
#include "core/Edit.h"
#include "media/MediaPool.h"
#include <cmath>
#include <cstring>

namespace rc {

float crossfadeGain(const QString &type, double p, bool fadingIn)
{
    p = std::clamp(p, 0.0, 1.0);
    double x = fadingIn ? p : 1.0 - p;
    if (type == "constantGain")
        return float(x);
    if (type == "exponentialFade")
        return float(x <= 0 ? 0 : std::pow(10.0, (x - 1.0) * 2.0) * x);   // ~-40 dB curve
    return float(std::sin(x * M_PI / 2));   // constant power
}

void AudioMixer::reset()
{
    for (auto &[k, p] : m_procs)
        if (p) p->reset();
    m_nextPos = -1;
    if (m_nested)
        m_nested->reset();
}

void AudioMixer::runChain(const QString &owner, const std::vector<EffectInstance> &chain, float *buf, int frames, int channels, int rate,
                          Ticks localStart, const float *sidechain)
{
    for (const auto &e : chain) {
        if (!e.enabled || e.type == "volume" || e.type == "panner")
            continue;
        const QString key = owner + "/" + e.uid;
        auto it = m_procs.find(key);
        if (it == m_procs.end())
            it = m_procs.emplace(key, createAudioProcessor(e.type)).first;
        if (!it->second)
            continue;
        AudioBlockInfo info;
        info.sampleRate = rate;
        info.channels = channels;
        info.localStart = localStart;
        info.sidechain = sidechain;
        it->second->process(buf, frames, e, info);
    }
}

void AudioMixer::mixMedia(const MediaItem &item, int rate, int channels, int64_t pos, int frames, float *out)
{
    std::memset(out, 0, sizeof(float) * size_t(frames) * channels);
    if (!item.info.hasAudio)
        return;
    std::vector<float> src;
    int sc = MediaPool::instance().readAudio(item.path, 0, rate, pos, frames, src);
    if (sc <= 0)
        return;
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c)
            out[i * channels + c] = src[size_t(i) * sc + std::min(c, sc - 1)];
}

void AudioMixer::renderClip(const Project &project, const Sequence &seq, const Track &track, const Clip &clip, int64_t pos, int frames,
                            float *out, float gainScale, int depth)
{
    const int rate = seq.sampleRate;
    const int outCh = seq.audioChannels;
    // Active range including transition handles.
    Ticks a0 = clip.start, a1 = clip.end();
    for (const auto &tr : track.transitions) {
        TimeRange r = edit::transitionRange(track, tr);
        if (tr.clipA == clip.id) a1 = std::max(a1, r.end);
        if (tr.clipB == clip.id) a0 = std::min(a0, r.start);
    }
    const int64_t s0 = std::max<int64_t>(pos, ticksToSamples(a0, rate));
    const int64_t s1 = std::min<int64_t>(pos + frames, ticksToSamples(a1, rate));
    if (s1 <= s0)
        return;
    const int n = int(s1 - s0);
    const int off = int(s0 - pos);

    // 1. Source samples into `buf` (outCh channels).
    std::vector<float> buf(size_t(n) * outCh, 0.f);
    const Ticks tStart = samplesToTicks(s0, rate);
    if (clip.source == ClipSource::Media) {
        const MediaItem *m = project.mediaItem(clip.mediaId);
        if (!m || !m->info.hasAudio)
            return;
        std::vector<float> src;
        const bool simple = clip.speed == 1.0 && !clip.reverse;
        if (simple) {
            int64_t srcPos = ticksToSamples(clip.sourceAt(tStart), rate);
            int sc = MediaPool::instance().readAudio(m->path, clip.audioStream, rate, srcPos, n, src);
            if (sc <= 0)
                return;
            for (int i = 0; i < n; ++i)
                for (int c = 0; c < outCh; ++c) {
                    int sch = clip.channelMap >= 0 ? std::min(clip.channelMap, sc - 1) : std::min(c, sc - 1);
                    buf[size_t(i) * outCh + c] = src[size_t(i) * sc + sch];
                }
        } else {
            // Variable speed / reverse: read the covered source span and interpolate.
            const Ticks tEnd = samplesToTicks(s1, rate);
            int64_t p0 = ticksToSamples(clip.sourceAt(tStart), rate);
            int64_t p1 = ticksToSamples(clip.sourceAt(tEnd), rate);
            int64_t lo = std::min(p0, p1) - 2, hi = std::max(p0, p1) + 2;
            int span = int(std::min<int64_t>(hi - lo, int64_t(rate) * 60));
            int sc = MediaPool::instance().readAudio(m->path, clip.audioStream, rate, lo, span, src);
            if (sc <= 0)
                return;
            for (int i = 0; i < n; ++i) {
                double sp = double(clip.sourceAt(samplesToTicks(s0 + i, rate))) / double(kTicksPerSecond / rate) - lo;
                int i0 = std::clamp(int(sp), 0, span - 1), i1 = std::min(span - 1, i0 + 1);
                float fr = float(sp - std::floor(sp));
                for (int c = 0; c < outCh; ++c) {
                    int sch = clip.channelMap >= 0 ? std::min(clip.channelMap, sc - 1) : std::min(c, sc - 1);
                    buf[size_t(i) * outCh + c] = src[size_t(i0) * sc + sch] * (1 - fr) + src[size_t(i1) * sc + sch] * fr;
                }
            }
            if (clip.maintainPitch && clip.speed != 1.0) {
                // Compensate the pitch change of resampling.
                EffectInstance ps = EffectInstance::create("pitch");
                ps.uid = "maintainPitch";
                ps.param("semitones").setStaticValue(Value(-12.0 * std::log2(clip.speed)));
                runChain(clip.id, {ps}, buf.data(), n, outCh, rate, tStart - clip.start);
            }
        }
    } else if (clip.source == ClipSource::Sequence) {
        const Sequence *nested = project.sequence(clip.mediaId);
        if (!nested || depth > 6 || nested->id == seq.id)
            return;
        if (!m_nested)
            m_nested = std::make_unique<AudioMixer>();
        int64_t srcPos = ticksToSamples(clip.sourceAt(tStart), rate);
        std::vector<float> tmp(size_t(n) * nested->audioChannels);
        m_nested->mix(project, *nested, srcPos, n, tmp.data());
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < outCh; ++c)
                buf[size_t(i) * outCh + c] = tmp[size_t(i) * nested->audioChannels + std::min(c, nested->audioChannels - 1)];
    } else if (clip.source == ClipSource::Bars) {
        // 1 kHz tone at -20 dBFS.
        for (int i = 0; i < n; ++i) {
            float v = 0.1f * float(std::sin(2 * M_PI * 1000.0 * double(s0 + i) / rate));
            for (int c = 0; c < outCh; ++c) buf[size_t(i) * outCh + c] = v;
        }
    } else {
        return;
    }

    // 2. Clip effects.
    runChain(clip.id, clip.effects, buf.data(), n, outCh, rate, tStart - clip.start);

    // 3. Volume keyframes, pan, transitions.
    const EffectInstance *vol = clip.effect("volume");
    const EffectInstance *pan = clip.effect("panner");
    const bool bypass = vol && vol->value("bypass", 0).b();
    const Transition *trIn = nullptr, *trOut = nullptr;
    TimeRange rIn, rOut;
    for (const auto &tr : track.transitions) {
        if (tr.clipB == clip.id) { trIn = &tr; rIn = edit::transitionRange(track, tr); }
        if (tr.clipA == clip.id) { trOut = &tr; rOut = edit::transitionRange(track, tr); }
    }
    const int step = 64;
    for (int i0 = 0; i0 < n; i0 += step) {
        const int i1 = std::min(n, i0 + step);
        const Ticks t = samplesToTicks(s0 + i0, rate);
        const Ticks local = t - clip.start;
        float g = gainScale;
        if (vol && vol->enabled && !bypass)
            g *= dbToGain(vol->f("level", local));
        if (trIn && rIn.contains(t))
            g *= crossfadeGain(trIn->effect.type, double(t - rIn.start) / std::max<Ticks>(1, rIn.duration()), true);
        if (trOut && rOut.contains(t))
            g *= crossfadeGain(trOut->effect.type, double(t - rOut.start) / std::max<Ticks>(1, rOut.duration()), false);
        if (t < clip.start && !trIn) g = 0;
        if (t >= clip.end() && !trOut) g = 0;
        float gl = g, gr = g;
        if (pan && pan->enabled && outCh >= 2) {
            double p = std::clamp(pan->f("pan", local) / 100.0, -1.0, 1.0);
            double a = (p + 1) * M_PI / 4;
            gl *= float(std::cos(a) * M_SQRT2);
            gr *= float(std::sin(a) * M_SQRT2);
            gl = std::min(gl, g);
            gr = std::min(gr, g);
        }
        for (int i = i0; i < i1; ++i) {
            float *o = out + size_t(off + i) * outCh;
            const float *b = buf.data() + size_t(i) * outCh;
            if (outCh >= 2) {
                o[0] += b[0] * gl;
                o[1] += b[1] * gr;
                for (int c = 2; c < outCh; ++c) o[c] += b[c] * g;
            } else {
                o[0] += b[0] * g;
            }
        }
    }
}

void AudioMixer::mixTrack(const Project &project, const Sequence &seq, const Track &track, int ti, int64_t pos, int frames, float *out, int depth)
{
    const int rate = seq.sampleRate;
    const Ticks t0 = samplesToTicks(pos, rate);
    const Ticks t1 = samplesToTicks(pos + frames, rate);
    for (const auto &clip : track.clips) {
        if (!clip.enabled)
            continue;
        // Quick reject with a margin for transition handles.
        Ticks margin = 0;
        for (const auto &tr : track.transitions)
            if (tr.clipA == clip.id || tr.clipB == clip.id)
                margin = std::max(margin, tr.duration);
        if (clip.end() + margin <= t0 || clip.start - margin >= t1)
            continue;
        renderClip(project, seq, track, clip, pos, frames, out, 1.0f, depth);
    }
    (void)ti;
}

static void measure(const float *buf, int frames, int ch, MeterLevels &m)
{
    m.peak.assign(ch, 0.f);
    m.rms.assign(ch, 0.f);
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < ch; ++c) {
            float v = buf[size_t(i) * ch + c];
            m.peak[c] = std::max(m.peak[c], std::abs(v));
            m.rms[c] += v * v;
        }
    for (int c = 0; c < ch; ++c)
        m.rms[c] = frames ? std::sqrt(m.rms[c] / frames) : 0.f;
}

void AudioMixer::mix(const Project &project, const Sequence &seq, int64_t pos, int frames, float *out)
{
    const int ch = std::max(1, seq.audioChannels);
    std::memset(out, 0, sizeof(float) * size_t(frames) * ch);
    if (frames <= 0)
        return;
    if (m_nextPos != pos)
        reset();
    m_nextPos = pos + frames;
    bool anySolo = false;
    for (const auto &t : seq.audioTracks)
        anySolo = anySolo || t.solo;
    m_tracks.resize(seq.audioTracks.size());
    std::vector<float> tb(size_t(frames) * ch);
    for (int ti = 0; ti < (int)seq.audioTracks.size(); ++ti) {
        const Track &track = seq.audioTracks[ti];
        std::fill(tb.begin(), tb.end(), 0.f);
        const bool audible = m_forceTrack >= 0 ? ti == m_forceTrack : (!track.muted && (!anySolo || track.solo));
        mixTrack(project, seq, track, ti, pos, frames, tb.data(), 0);
        // Track inserts (with sidechain for ducking).
        const float *side = nullptr;
        for (const auto &e : track.effects)
            if (e.type == "ducking" && e.enabled) {
                int sti = e.value("sidechainTrack", 0).i() - 1;
                if (sti >= 0 && sti < (int)seq.audioTracks.size()) {
                    auto it = m_trackBuffers.find(seq.audioTracks[sti].id);
                    if (it != m_trackBuffers.end() && it->second.size() == tb.size())
                        side = it->second.data();
                }
            }
        runChain(track.id, track.effects, tb.data(), frames, ch, seq.sampleRate, samplesToTicks(pos, seq.sampleRate), side);
        float g = dbToGain(track.volumeDb);
        float gl = g, gr = g;
        if (ch >= 2 && track.pan != 0) {
            double a = (std::clamp(track.pan / 100.0, -1.0, 1.0) + 1) * M_PI / 4;
            gl = std::min(g, float(g * std::cos(a) * M_SQRT2));
            gr = std::min(g, float(g * std::sin(a) * M_SQRT2));
        }
        for (int i = 0; i < frames; ++i) {
            tb[size_t(i) * ch] *= gl;
            if (ch >= 2) tb[size_t(i) * ch + 1] *= gr;
            for (int c = 2; c < ch; ++c) tb[size_t(i) * ch + c] *= g;
        }
        m_trackBuffers[track.id] = tb;
        measure(tb.data(), frames, ch, m_tracks[ti]);
        if (!audible)
            continue;
        for (size_t k = 0; k < tb.size(); ++k)
            out[k] += tb[k];
    }
    runChain(seq.id + "/master", seq.masterEffects, out, frames, ch, seq.sampleRate, samplesToTicks(pos, seq.sampleRate));
    const float mg = dbToGain(seq.masterVolumeDb);
    for (size_t k = 0; k < size_t(frames) * ch; ++k)
        out[k] *= mg;
    measure(out, frames, ch, m_master);
}

} // namespace rc
