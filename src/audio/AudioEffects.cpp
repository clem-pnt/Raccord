#include "AudioEffects.h"
#include <cmath>
#include <complex>
#include <cstring>

namespace rc {

// ---- Biquad ------------------------------------------------------------------------

void Biquad::lowShelf(double fs, double f, double gainDb, double q)
{
    double A = std::pow(10, gainDb / 40), w = 2 * M_PI * f / fs, c = std::cos(w), s = std::sin(w), al = s / (2 * q);
    double sa = 2 * std::sqrt(A) * al;
    double a0 = (A + 1) + (A - 1) * c + sa;
    b0 = A * ((A + 1) - (A - 1) * c + sa) / a0;
    b1 = 2 * A * ((A - 1) - (A + 1) * c) / a0;
    b2 = A * ((A + 1) - (A - 1) * c - sa) / a0;
    a1 = -2 * ((A - 1) + (A + 1) * c) / a0;
    a2 = ((A + 1) + (A - 1) * c - sa) / a0;
}
void Biquad::highShelf(double fs, double f, double gainDb, double q)
{
    double A = std::pow(10, gainDb / 40), w = 2 * M_PI * f / fs, c = std::cos(w), s = std::sin(w), al = s / (2 * q);
    double sa = 2 * std::sqrt(A) * al;
    double a0 = (A + 1) - (A - 1) * c + sa;
    b0 = A * ((A + 1) + (A - 1) * c + sa) / a0;
    b1 = -2 * A * ((A - 1) + (A + 1) * c) / a0;
    b2 = A * ((A + 1) + (A - 1) * c - sa) / a0;
    a1 = 2 * ((A - 1) - (A + 1) * c) / a0;
    a2 = ((A + 1) - (A - 1) * c - sa) / a0;
}
void Biquad::peak(double fs, double f, double gainDb, double q)
{
    double A = std::pow(10, gainDb / 40), w = 2 * M_PI * f / fs, al = std::sin(w) / (2 * q), c = std::cos(w);
    double a0 = 1 + al / A;
    b0 = (1 + al * A) / a0;
    b1 = -2 * c / a0;
    b2 = (1 - al * A) / a0;
    a1 = -2 * c / a0;
    a2 = (1 - al / A) / a0;
}
void Biquad::lowpass(double fs, double f, double q)
{
    double w = 2 * M_PI * std::min(f, fs * 0.49) / fs, c = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
    b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = b0; a1 = -2 * c / a0; a2 = (1 - al) / a0;
}
void Biquad::highpass(double fs, double f, double q)
{
    double w = 2 * M_PI * std::min(f, fs * 0.49) / fs, c = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
    b0 = (1 + c) / 2 / a0; b1 = -(1 + c) / a0; b2 = b0; a1 = -2 * c / a0; a2 = (1 - al) / a0;
}
void Biquad::bandpass(double fs, double f, double q)
{
    double w = 2 * M_PI * std::min(f, fs * 0.49) / fs, c = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
    b0 = al / a0; b1 = 0; b2 = -al / a0; a1 = -2 * c / a0; a2 = (1 - al) / a0;
}

namespace {

Ticks tickAt(const AudioBlockInfo &info, int i) { return info.localStart + samplesToTicks(i, info.sampleRate); }

class Eq : public AudioProcessor {
    Biquad lo, mid, hi;
    double key[7] = {-1};
public:
    void process(float *buf, int frames, const EffectInstance &e, const AudioBlockInfo &info) override
    {
        Ticks t = info.localStart;
        double k[7] = {e.f("lowFreq", t), e.f("lowGain", t), e.f("midFreq", t), e.f("midGain", t), e.f("midQ", t), e.f("highFreq", t), e.f("highGain", t)};
        if (std::memcmp(k, key, sizeof k) != 0) {
            lo.lowShelf(info.sampleRate, k[0], k[1]);
            mid.peak(info.sampleRate, k[2], k[3], std::max(0.1, k[4]));
            hi.highShelf(info.sampleRate, k[5], k[6]);
            std::memcpy(key, k, sizeof k);
        }
        for (int i = 0; i < frames; ++i)
            for (int c = 0; c < info.channels && c < 8; ++c) {
                float &x = buf[i * info.channels + c];
                x = hi.run(mid.run(lo.run(x, c), c), c);
            }
    }
    void reset() override { lo.reset(); mid.reset(); hi.reset(); }
};

class Filter : public AudioProcessor {
    Biquad f1, f2;
    bool high;
    double last = -1;
public:
    explicit Filter(bool h) : high(h) {}
    void process(float *buf, int frames, const EffectInstance &e, const AudioBlockInfo &info) override
    {
        double fc = e.f("cutoff", info.localStart);
        if (fc != last) {
            if (high) { f1.highpass(info.sampleRate, fc); f2.highpass(info.sampleRate, fc); }
            else { f1.lowpass(info.sampleRate, fc); f2.lowpass(info.sampleRate, fc); }
            last = fc;
        }
        for (int i = 0; i < frames; ++i)
            for (int c = 0; c < info.channels && c < 8; ++c) {
                float &x = buf[i * info.channels + c];
                x = f2.run(f1.run(x, c), c);
            }
    }
    void reset() override { f1.reset(); f2.reset(); }
};

class Dynamics : public AudioProcessor {
public:
    enum Mode { Compressor, Limiter, Gate };
    explicit Dynamics(Mode m) : mode(m) {}
    void process(float *buf, int frames, const EffectInstance &e, const AudioBlockInfo &info) override
    {
        const Ticks t = info.localStart;
        const double fs = info.sampleRate;
        double thr, ratio = 1, att, rel, makeup = 0;
        if (mode == Compressor) {
            thr = e.f("threshold", t); ratio = std::max(1.0, e.f("ratio", t));
            att = e.f("attack", t); rel = e.f("release", t); makeup = e.f("makeup", t);
        } else if (mode == Limiter) {
            thr = e.f("ceiling", t); ratio = 1000; att = 0.1; rel = e.f("release", t);
        } else {
            thr = e.f("threshold", t); att = e.f("attack", t); rel = e.f("release", t);
        }
        const double ka = std::exp(-1.0 / (std::max(0.05, att) * 0.001 * fs));
        const double kr = std::exp(-1.0 / (std::max(1.0, rel) * 0.001 * fs));
        const float mk = dbToGain(makeup);
        const int ch = info.channels;
        // Limiter uses a small look-ahead delay for transparent peak control.
        const int look = mode == Limiter ? int(fs * 0.002) : 0;
        if ((int)delay.size() != (look + 1) * ch) { delay.assign(size_t(look + 1) * ch, 0.f); dpos = 0; }
        for (int i = 0; i < frames; ++i) {
            float peak = 0;
            for (int c = 0; c < ch; ++c)
                peak = std::max(peak, std::abs(buf[i * ch + c]));
            double lvl = gainToDb(peak);
            double target;
            if (mode == Gate) {
                target = lvl > thr ? 1.0 : 0.0;
            } else {
                double over = lvl - thr;
                target = over > 0 ? dbToGain(-over * (1 - 1 / ratio)) : 1.0;
            }
            double k = target < env ? ka : kr;
            if (mode == Gate) k = target > env ? ka : kr;
            env = target + (env - target) * k;
            for (int c = 0; c < ch; ++c) {
                float x = buf[i * ch + c];
                if (look > 0) {
                    float d = delay[size_t(dpos) * ch + c];
                    delay[size_t(dpos) * ch + c] = x;
                    x = d;
                }
                float y = float(x * env) * mk;
                if (mode == Limiter) {
                    float ceil = dbToGain(thr);
                    y = std::clamp(y, -ceil, ceil);
                }
                buf[i * ch + c] = y;
            }
            if (look > 0) dpos = (dpos + 1) % (look + 1);
        }
    }
    void reset() override { env = 1.0; std::fill(delay.begin(), delay.end(), 0.f); }
private:
    Mode mode;
    double env = 1.0;
    std::vector<float> delay;
    int dpos = 0;
};

// Spectral gate noise reduction (STFT 1024, hop 256, Hann, noise floor estimated adaptively).
class Denoise : public AudioProcessor {
    static constexpr int N = 1024, H = 256;
    struct Ch {
        std::vector<float> in, out;
        std::vector<float> noise;
        int filled = 0;
    };
    std::vector<Ch> chans;
    std::vector<float> window;
    std::vector<float> outQueue;
    static void fft(std::vector<std::complex<float>> &a, bool inv)
    {
        const int n = (int)a.size();
        for (int i = 1, j = 0; i < n; ++i) {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }
        for (int len = 2; len <= n; len <<= 1) {
            float ang = 2 * float(M_PI) / len * (inv ? 1 : -1);
            std::complex<float> wl(std::cos(ang), std::sin(ang));
            for (int i = 0; i < n; i += len) {
                std::complex<float> w(1);
                for (int j = 0; j < len / 2; ++j) {
                    auto u = a[i + j], v = a[i + j + len / 2] * w;
                    a[i + j] = u + v;
                    a[i + j + len / 2] = u - v;
                    w *= wl;
                }
            }
        }
        if (inv)
            for (auto &x : a) x /= float(n);
    }
public:
    void process(float *buf, int frames, const EffectInstance &e, const AudioBlockInfo &info) override
    {
        const int ch = info.channels;
        if ((int)chans.size() != ch) {
            chans.assign(ch, Ch());
            for (auto &c : chans) {
                c.in.assign(N, 0.f);
                c.out.assign(N + H, 0.f);
                c.noise.assign(N / 2 + 1, 0.f);
            }
            window.resize(N);
            for (int i = 0; i < N; ++i) window[i] = 0.5f - 0.5f * std::cos(2 * float(M_PI) * i / N);
        }
        const float amount = float(e.f("amount", info.localStart) / 100.0);
        const float sens = float(e.f("sensitivity", info.localStart) / 100.0);
        std::vector<std::complex<float>> spec(N);
        for (int c = 0; c < ch; ++c) {
            Ch &C = chans[c];
            for (int i = 0; i < frames; ++i) {
                // Shift sample in; latency = N - H samples.
                C.in[C.filled + (N - H)] = buf[i * ch + c];
                ++C.filled;
                buf[i * ch + c] = C.out[C.filled - 1];
                if (C.filled == H) {
                    C.filled = 0;
                    for (int k = 0; k < N; ++k) spec[k] = C.in[k] * window[k];
                    fft(spec, false);
                    for (int k = 0; k <= N / 2; ++k) {
                        float mag = std::abs(spec[k]);
                        // Minimum statistics style tracking: fast down, slow up.
                        float &nf = C.noise[k];
                        nf = mag < nf || nf == 0 ? mag * 0.3f + nf * 0.7f : nf * 1.002f + mag * 0.0005f;
                        float thr = nf * (1.5f + 4.f * sens);
                        float g = mag > thr ? 1.f : std::pow(mag / std::max(thr, 1e-9f), 2.f);
                        g = 1.f - amount * (1.f - g);
                        spec[k] *= g;
                        if (k > 0 && k < N / 2) spec[N - k] = std::conj(spec[k]);
                    }
                    fft(spec, true);
                    // Overlap-add (Hann^2 with hop N/4 sums to 1.5).
                    std::memmove(C.out.data(), C.out.data() + H, sizeof(float) * N);
                    std::fill(C.out.begin() + N, C.out.end(), 0.f);
                    for (int k = 0; k < N; ++k) C.out[k] += spec[k].real() * window[k] / 1.5f;
                    std::memmove(C.in.data(), C.in.data() + H, sizeof(float) * (N - H));
                }
            }
        }
    }
    void reset() override { chans.clear(); }
};

class DeEsser : public AudioProcessor {
    Biquad band;
    double last = -1, env = 0;
public:
    void process(float *buf, int frames, const EffectInstance &e, const AudioBlockInfo &info) override
    {
        double f = e.f("frequency", info.localStart), thr = e.f("threshold", info.localStart);
        if (f != last) { band.bandpass(info.sampleRate, f, 1.5); last = f; }
        const int ch = info.channels;
        const double k = std::exp(-1.0 / (0.005 * info.sampleRate));
        for (int i = 0; i < frames; ++i) {
            float mono = 0;
            for (int c = 0; c < ch; ++c) mono += buf[i * ch + c];
            float s = band.run(mono / ch, 0);
            env = std::max(double(std::abs(s)), env * k);
            double over = gainToDb(env) - thr;
            float g = over > 0 ? dbToGain(-over * 0.75) : 1.f;
            for (int c = 0; c < ch; ++c) buf[i * ch + c] *= g;
        }
    }
    void reset() override { band.reset(); env = 0; }
};

// Freeverb-style reverb.
class Reverb : public AudioProcessor {
    struct Comb { std::vector<float> b; int i = 0; float store = 0; };
    struct AP { std::vector<float> b; int i = 0; };
    std::vector<Comb> combs[2];
    std::vector<AP> aps[2];
    int rate = 0;
    void init(int fs)
    {
        static const int cl[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static const int al[4] = {556, 441, 341, 225};
        for (int s = 0; s < 2; ++s) {
            combs[s].clear();
            aps[s].clear();
            for (int k : cl) combs[s].push_back({std::vector<float>(size_t((k + s * 23) * fs / 44100), 0.f)});
            for (int k : al) aps[s].push_back({std::vector<float>(size_t((k + s * 23) * fs / 44100), 0.f)});
        }
        rate = fs;
    }
public:
    void process(float *buf, int frames, const EffectInstance &e, const AudioBlockInfo &info) override
    {
        if (rate != info.sampleRate) init(info.sampleRate);
        const float room = 0.7f + float(e.f("room", info.localStart) / 100.0) * 0.28f;
        const float damp = float(e.f("damping", info.localStart) / 100.0) * 0.4f;
        const float mix = float(e.f("mix", info.localStart) / 100.0);
        const int ch = info.channels;
        for (int i = 0; i < frames; ++i) {
            float in = 0;
            for (int c = 0; c < ch; ++c) in += buf[i * ch + c];
            in = in / ch * 0.015f;
            for (int s = 0; s < 2; ++s) {
                float out = 0;
                for (auto &cb : combs[s]) {
                    float y = cb.b[cb.i];
                    cb.store = y * (1 - damp) + cb.store * damp;
                    cb.b[cb.i] = in + cb.store * room;
                    cb.i = (cb.i + 1) % (int)cb.b.size();
                    out += y;
                }
                for (auto &ap : aps[s]) {
                    float bo = ap.b[ap.i];
                    float y = -out + bo;
                    ap.b[ap.i] = out + bo * 0.5f;
                    ap.i = (ap.i + 1) % (int)ap.b.size();
                    out = y;
                }
                int c = ch == 1 ? 0 : s;
                if (c < ch) buf[i * ch + c] = buf[i * ch + c] * (1 - mix) + out * mix * 3.f;
            }
        }
    }
    void reset() override { rate = 0; }
};

class Delay : public AudioProcessor {
    std::vector<float> line;
    int pos = 0;
public:
    void process(float *buf, int frames, const EffectInstance &e, const AudioBlockInfo &info) override
    {
        const int ch = info.channels;
        const int d = std::max(1, int(e.f("time", info.localStart) * 0.001 * info.sampleRate));
        const float fb = float(e.f("feedback", info.localStart) / 100.0), mix = float(e.f("mix", info.localStart) / 100.0);
        if ((int)line.size() != (d + 1) * ch) { line.assign(size_t(d + 1) * ch, 0.f); pos = 0; }
        for (int i = 0; i < frames; ++i) {
            for (int c = 0; c < ch; ++c) {
                float &slot = line[size_t(pos) * ch + c];
                float delayed = slot;
                float x = buf[i * ch + c];
                slot = x + delayed * fb;
                buf[i * ch + c] = x * (1 - mix * 0.5f) + delayed * mix;
            }
            pos = (pos + 1) % (d + 1);
        }
    }
    void reset() override { std::fill(line.begin(), line.end(), 0.f); }
};

// Granular pitch shifter (two crossfaded read heads).
class Pitch : public AudioProcessor {
    std::vector<float> ring;
    double phase = 0;
    int w = 0;
public:
    void process(float *buf, int frames, const EffectInstance &e, const AudioBlockInfo &info) override
    {
        const int ch = info.channels;
        const double ratio = std::pow(2.0, e.f("semitones", info.localStart) / 12.0);
        const int L = int(info.sampleRate * 0.05);   // 50 ms grains
        const int R = L * 4;
        if ((int)ring.size() != R * ch) { ring.assign(size_t(R) * ch, 0.f); w = 0; phase = 0; }
        if (std::abs(ratio - 1.0) < 1e-4) return;
        for (int i = 0; i < frames; ++i) {
            for (int c = 0; c < ch; ++c) ring[size_t(w) * ch + c] = buf[i * ch + c];
            phase += 1.0 - ratio;
            if (phase < 0) phase += L;
            if (phase >= L) phase -= L;
            double d1 = phase, d2 = std::fmod(phase + L / 2.0, L);
            float g1 = float(std::sin(M_PI * d1 / L)), g2 = float(std::sin(M_PI * d2 / L));
            for (int c = 0; c < ch; ++c) {
                auto tap = [&](double d) {
                    double p = w - d - 1;
                    while (p < 0) p += R;
                    int i0 = int(p) % R, i1 = (i0 + 1) % R;
                    float f = float(p - std::floor(p));
                    return ring[size_t(i0) * ch + c] * (1 - f) + ring[size_t(i1) * ch + c] * f;
                };
                buf[i * ch + c] = tap(d1) * g1 * g1 + tap(d2) * g2 * g2;
            }
            w = (w + 1) % R;
        }
    }
    void reset() override { std::fill(ring.begin(), ring.end(), 0.f); }
};

class ChannelTools : public AudioProcessor {
public:
    void process(float *buf, int frames, const EffectInstance &e, const AudioBlockInfo &info) override
    {
        const int ch = info.channels;
        if (ch < 2) return;
        const int mode = e.value("mode", 0).i();
        for (int i = 0; i < frames; ++i) {
            float &l = buf[i * ch], &r = buf[i * ch + 1];
            switch (mode) {
            case 1: std::swap(l, r); break;
            case 2: r = l; break;
            case 3: l = r; break;
            case 4: l = r = (l + r) * 0.5f; break;
            case 5: r = -r; break;
            default: break;
            }
        }
    }
};

class Ducking : public AudioProcessor {
    double env = 0, gain = 1;
public:
    void process(float *buf, int frames, const EffectInstance &e, const AudioBlockInfo &info) override
    {
        if (!info.sidechain) return;
        const int ch = info.channels;
        const double amount = e.f("amount", info.localStart), sens = e.f("sensitivity", info.localStart);
        const double fade = std::max(10.0, e.f("fade", info.localStart)) * 0.001 * info.sampleRate;
        const double kEnv = std::exp(-1.0 / (0.05 * info.sampleRate));
        const double kGain = std::exp(-1.0 / fade);
        const double duck = dbToGain(amount);
        for (int i = 0; i < frames; ++i) {
            float p = 0;
            for (int c = 0; c < ch; ++c) p = std::max(p, std::abs(info.sidechain[i * ch + c]));
            env = std::max(double(p), env * kEnv);
            double target = gainToDb(env) > sens ? duck : 1.0;
            gain = target + (gain - target) * kGain;
            for (int c = 0; c < ch; ++c) buf[i * ch + c] *= float(gain);
        }
    }
    void reset() override { env = 0; gain = 1; }
};

} // namespace

std::unique_ptr<AudioProcessor> createAudioProcessor(const QString &type)
{
    if (type == "eq") return std::make_unique<Eq>();
    if (type == "highpass") return std::make_unique<Filter>(true);
    if (type == "lowpass") return std::make_unique<Filter>(false);
    if (type == "compressor") return std::make_unique<Dynamics>(Dynamics::Compressor);
    if (type == "limiter") return std::make_unique<Dynamics>(Dynamics::Limiter);
    if (type == "noiseGate") return std::make_unique<Dynamics>(Dynamics::Gate);
    if (type == "denoise") return std::make_unique<Denoise>();
    if (type == "deesser") return std::make_unique<DeEsser>();
    if (type == "reverb") return std::make_unique<Reverb>();
    if (type == "delay") return std::make_unique<Delay>();
    if (type == "pitch") return std::make_unique<Pitch>();
    if (type == "channelSwap") return std::make_unique<ChannelTools>();
    if (type == "ducking") return std::make_unique<Ducking>();
    return nullptr;
}

} // namespace rc
