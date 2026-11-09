#include "Analysis.h"
#include "media/AudioReader.h"
#include "media/FFmpeg.h"
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cmath>
#include <complex>
#include <numeric>

namespace rc::ai {

bool scanFrames(const QString &path, Ticks in, Ticks out, int w, int h, bool gray,
                const std::function<bool(Ticks, const uint8_t *, int)> &cb, QString *error)
{
    ff::initLogging();
    ff::FormatCtx fmt = ff::openInput(path, error);
    if (!fmt)
        return false;
    const int si = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (si < 0) {
        if (error) *error = QObject::tr("No video stream");
        return false;
    }
    AVStream *st = fmt->streams[si];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    ff::CodecCtx dec(avcodec_alloc_context3(codec));
    avcodec_parameters_to_context(dec.get(), st->codecpar);
    dec->thread_count = 0;
    dec->skip_loop_filter = AVDISCARD_ALL;   // analysis does not need deblocking
    if (avcodec_open2(dec.get(), codec, nullptr) < 0) {
        if (error) *error = QObject::tr("Cannot open decoder");
        return false;
    }
    const int64_t startPts = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
    const AVRational tb = st->time_base;
    if (in > 0) {
        const int64_t target = startPts + av_rescale_q(int64_t(ticksToSeconds(in) * AV_TIME_BASE), AV_TIME_BASE_Q, tb);
        av_seek_frame(fmt.get(), si, target, AVSEEK_FLAG_BACKWARD);
    }
    ff::Packet pkt = ff::makePacket();
    ff::Frame frame = ff::makeFrame();
    ff::Sws sws;
    AVPixelFormat swsFmt = AV_PIX_FMT_NONE;
    int swsW = 0, swsH = 0;
    std::vector<uint8_t> buf(size_t(w) * h * (gray ? 1 : 3));
    const int stride = w * (gray ? 1 : 3);
    bool running = true;
    auto handle = [&]() {
        while (running && avcodec_receive_frame(dec.get(), frame.get()) == 0) {
            const int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
            const Ticks t = rationalToTicks(pts - startPts, tb.num, tb.den);
            if (t >= out) { running = false; break; }
            if (t + kTicksPerSecond / 1000 < in) { av_frame_unref(frame.get()); continue; }
            if (!sws || swsFmt != frame->format || swsW != frame->width || swsH != frame->height) {
                sws.reset(sws_getContext(frame->width, frame->height, AVPixelFormat(frame->format), w, h,
                                         gray ? AV_PIX_FMT_GRAY8 : AV_PIX_FMT_RGB24, SWS_AREA, nullptr, nullptr, nullptr));
                swsFmt = AVPixelFormat(frame->format);
                swsW = frame->width;
                swsH = frame->height;
            }
            uint8_t *dst[1] = {buf.data()};
            int dstStride[1] = {stride};
            sws_scale(sws.get(), frame->data, frame->linesize, 0, frame->height, dst, dstStride);
            av_frame_unref(frame.get());
            if (!cb(t, buf.data(), stride)) running = false;
        }
    };
    while (running && av_read_frame(fmt.get(), pkt.get()) >= 0) {
        if (pkt->stream_index == si)
            avcodec_send_packet(dec.get(), pkt.get());
        av_packet_unref(pkt.get());
        handle();
    }
    if (running) {
        avcodec_send_packet(dec.get(), nullptr);
        handle();
    }
    return true;
}

// ---------------------------------------------------------------------------

std::vector<Ticks> detectScenes(const QString &path, Ticks in, Ticks out, double sensitivity, const Progress &progress, QString *error)
{
    constexpr int W = 96, H = 54, BINS = 16;
    std::vector<Ticks> cuts;
    std::vector<float> prevHist, prevPix;
    std::vector<double> scores;       // recent scores for the adaptive threshold
    Ticks lastCut = in;
    const Ticks minShot = kTicksPerSecond / 2;
    const double base = 0.55 - 0.4 * std::clamp(sensitivity, 0.0, 1.0);   // histogram distance threshold
    const Ticks span = std::max<Ticks>(1, out - in);
    scanFrames(path, in, out, W, H, false, [&](Ticks t, const uint8_t *d, int stride) {
        std::vector<float> hist(BINS * 3, 0.f), pix(W * H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const uint8_t *p = d + y * stride + x * 3;
                hist[p[0] * BINS / 256]++;
                hist[BINS + p[1] * BINS / 256]++;
                hist[2 * BINS + p[2] * BINS / 256]++;
                pix[y * W + x] = (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) / 255.f;
            }
        for (auto &v : hist) v /= float(W * H);
        if (!prevHist.empty()) {
            double hd = 0, pd = 0;
            for (int i = 0; i < BINS * 3; ++i) hd += std::abs(hist[i] - prevHist[i]);
            hd /= 6.0;   // 0..1
            for (int i = 0; i < W * H; ++i) pd += std::abs(pix[i] - prevPix[i]);
            pd /= W * H;
            const double score = 0.6 * hd + 0.4 * pd * 2.0;
            double mean = scores.empty() ? 0 : std::accumulate(scores.begin(), scores.end(), 0.0) / scores.size();
            if (score > base && score > mean * 3.0 + 0.04 && t - lastCut >= minShot) {
                cuts.push_back(t);
                lastCut = t;
            }
            scores.push_back(score);
            if (scores.size() > 24) scores.erase(scores.begin());
        }
        prevHist.swap(hist);
        prevPix.swap(pix);
        return !progress || progress(double(t - in) / span);
    }, error);
    return cuts;
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Audio sync

namespace {

void fft(std::vector<std::complex<double>> &a, bool inverse)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2 * M_PI / double(len) * (inverse ? 1 : -1);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse)
        for (auto &x : a) x /= double(n);
}

constexpr int kSyncRate = 8000;
constexpr int kEnvRate = 200;   // envelope samples per second (5 ms)

// Onset envelope: positive slope of log energy, zero mean / unit variance.
std::vector<double> onsetEnvelope(const QString &path, double maxSeconds, QString *error)
{
    std::vector<double> env;
    AudioReader r;
    if (!r.open(path, 0, kSyncRate, error))
        return env;
    const int ch = std::max(1, r.channels());
    const int win = kSyncRate / kEnvRate;
    const int64_t total = std::min<int64_t>(r.lengthSamples() > 0 ? r.lengthSamples() : int64_t(maxSeconds * kSyncRate),
                                            int64_t(maxSeconds * kSyncRate));
    std::vector<float> buf(size_t(kSyncRate) * ch);
    double prev = 0;
    for (int64_t pos = 0; pos < total; pos += kSyncRate) {
        const int n = int(std::min<int64_t>(kSyncRate, total - pos));
        r.read(pos, n, buf.data());
        for (int w0 = 0; w0 + win <= n; w0 += win) {
            double sum = 0;
            for (int i = 0; i < win; ++i)
                for (int c = 0; c < ch; ++c) {
                    const double v = buf[size_t(w0 + i) * ch + c];
                    sum += v * v;
                }
            const double e = std::log10(1e-9 + sum / (win * ch));
            env.push_back(std::max(0.0, e - prev));
            prev = e;
        }
    }
    if (env.empty())
        return env;
    const double mean = std::accumulate(env.begin(), env.end(), 0.0) / env.size();
    double var = 0;
    for (double &v : env) { v -= mean; var += v * v; }
    const double sd = std::sqrt(var / env.size());
    if (sd > 1e-12)
        for (double &v : env) v /= sd;
    return env;
}

} // namespace

std::vector<Ticks> audioSyncOffsets(const QStringList &paths, double maxSeconds, const Progress &progress, QString *error)
{
    std::vector<Ticks> out(paths.size(), 0);
    std::vector<std::vector<double>> envs;
    for (int i = 0; i < paths.size(); ++i) {
        QString err;
        envs.push_back(onsetEnvelope(paths[i], maxSeconds, &err));
        if (progress && !progress(0.8 * (i + 1) / paths.size()))
            return out;
    }
    if (envs.empty() || envs[0].empty()) {
        if (error) *error = QCoreApplication::translate("rc::ai", "The first clip has no usable audio.");
        return out;
    }
    const auto &ref = envs[0];
    for (int i = 1; i < (int)envs.size(); ++i) {
        const auto &b = envs[i];
        if (b.empty())
            continue;
        size_t n = 1;
        while (n < ref.size() + b.size()) n <<= 1;
        std::vector<std::complex<double>> A(n), B(n);
        for (size_t k = 0; k < ref.size(); ++k) A[k] = ref[k];
        for (size_t k = 0; k < b.size(); ++k) B[k] = b[k];
        fft(A, false);
        fft(B, false);
        for (size_t k = 0; k < n; ++k) A[k] *= std::conj(B[k]);
        fft(A, true);
        // c[k] = sum ref[m] b[m-k]: b starts k envelope samples after ref.
        // Normalise by the overlap so short overlaps at the edges do not win.
        double best = -1e300;
        long bestLag = 0;
        const long na = long(ref.size()), nb = long(b.size());
        for (long lag = -(nb - 1); lag <= na - 1; ++lag) {
            const long overlap = std::min(na, lag + nb) - std::max(0L, lag);
            if (overlap < std::min<long>(kEnvRate * 2, std::min(na, nb) / 2))
                continue;
            const double v = A[size_t(lag >= 0 ? lag : long(n) + lag)].real() / std::sqrt(double(overlap));
            if (v > best) { best = v; bestLag = lag; }
        }
        out[i] = secondsToTicks(double(bestLag) / kEnvRate);
        if (progress && !progress(0.8 + 0.2 * i / (envs.size() - 1)))
            return out;
    }
    return out;
}

std::vector<TimeRange> detectSilence(const QString &path, int audioStream, Ticks in, Ticks out, double thresholdDb,
                                     Ticks minDuration, Ticks padding, const Progress &progress, QString *error)
{
    std::vector<TimeRange> result;
    AudioReader r;
    constexpr int rate = 16000;
    if (!r.open(path, audioStream, rate, error))
        return result;
    const int ch = r.channels();
    const int win = rate / 100;   // 10 ms
    const int64_t s0 = int64_t(ticksToSeconds(in) * rate);
    const int64_t s1 = int64_t(ticksToSeconds(out) * rate);
    const double thr = std::pow(10.0, thresholdDb / 20.0);
    std::vector<float> buf(size_t(rate) * ch);
    int64_t silentStart = -1;
    auto close = [&](int64_t endSample) {
        if (silentStart < 0) return;
        Ticks a = secondsToTicks(double(silentStart) / rate), b = secondsToTicks(double(endSample) / rate);
        if (b - a >= minDuration) {
            a += (silentStart <= s0 ? 0 : padding);
            b -= (endSample >= s1 ? 0 : padding);
            if (b > a) result.push_back({a, b});
        }
        silentStart = -1;
    };
    // Hysteresis: smoothed RMS with a short hold avoids chopping between words.
    double env = 0;
    for (int64_t pos = s0; pos < s1; pos += rate) {
        const int n = int(std::min<int64_t>(rate, s1 - pos));
        r.read(pos, n, buf.data());
        for (int w0 = 0; w0 < n; w0 += win) {
            const int wn = std::min(win, n - w0);
            double sum = 0;
            for (int i = 0; i < wn; ++i)
                for (int c = 0; c < ch; ++c) {
                    const double v = buf[size_t(w0 + i) * ch + c];
                    sum += v * v;
                }
            const double rms = std::sqrt(sum / std::max(1, wn * ch));
            env = rms > env ? rms : env * 0.8 + rms * 0.2;
            const int64_t at = pos + w0;
            if (env < thr) {
                if (silentStart < 0) silentStart = at;
            } else {
                close(at);
            }
        }
        if (progress && !progress(double(pos - s0) / std::max<int64_t>(1, s1 - s0)))
            return {};
    }
    close(s1);
    return result;
}

// ---------------------------------------------------------------------------

std::vector<ReframeSample> reframeTrack(const QString &path, Ticks in, Ticks out, double smoothing, const Progress &progress, QString *error)
{
    constexpr int W = 160, H = 90;
    std::vector<ReframeSample> raw;
    std::vector<float> prev;
    const Ticks span = std::max<Ticks>(1, out - in);
    // Skin-tone and motion weighted saliency: a light-weight subject tracker.
    scanFrames(path, in, out, W, H, false, [&](Ticks t, const uint8_t *d, int stride) {
        std::vector<float> lum(W * H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const uint8_t *p = d + y * stride + x * 3;
                lum[y * W + x] = (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) / 255.f;
            }
        double sx = 0, sy = 0, sw = 0;
        for (int y = 1; y < H - 1; ++y)
            for (int x = 1; x < W - 1; ++x) {
                const int i = y * W + x;
                const uint8_t *p = d + y * stride + x * 3;
                const float gx = lum[i + 1] - lum[i - 1], gy = lum[i + W] - lum[i - W];
                const float edge = std::sqrt(gx * gx + gy * gy);
                const float motion = prev.empty() ? 0.f : std::abs(lum[i] - prev[i]);
                // Skin: r > g > b, enough chroma (YCbCr box).
                const float r = p[0], g = p[1], b = p[2];
                const float cb = 128 - 0.168736f * r - 0.331264f * g + 0.5f * b;
                const float cr = 128 + 0.5f * r - 0.418688f * g - 0.081312f * b;
                const float skin = (cr > 135 && cr < 175 && cb > 85 && cb < 135 && r > 60) ? 1.f : 0.f;
                // Centre bias: subjects are rarely on the border.
                const float bias = 1.0f - 0.5f * std::abs(x / float(W) - 0.5f);
                const float wgt = (edge * 0.6f + motion * 2.5f + skin * 0.35f) * bias;
                sx += wgt * x;
                sy += wgt * y;
                sw += wgt;
            }
        if (sw > 1e-3)
            raw.push_back({t, sx / sw / W, sy / sw / H});
        else
            raw.push_back({t, 0.5, 0.5});
        prev.swap(lum);
        return !progress || progress(double(t - in) / span);
    }, error);
    if (raw.empty())
        return raw;
    // Smooth: forward/backward exponential filter (zero phase), with a dead zone
    // so the virtual camera holds still for small movements.
    const double a = 0.02 + 0.25 * (1.0 - std::clamp(smoothing, 0.0, 1.0));
    std::vector<ReframeSample> s = raw;
    for (size_t i = 1; i < s.size(); ++i) {
        s[i].x = s[i - 1].x + a * (raw[i].x - s[i - 1].x);
        s[i].y = s[i - 1].y + a * (raw[i].y - s[i - 1].y);
    }
    for (size_t i = s.size() - 1; i-- > 0;) {
        s[i].x = s[i + 1].x + a * (s[i].x - s[i + 1].x);
        s[i].y = s[i + 1].y + a * (s[i].y - s[i + 1].y);
    }
    std::vector<ReframeSample> outS;
    double hold = s.front().x;
    for (size_t i = 0; i < s.size(); ++i) {
        if (std::abs(s[i].x - hold) > 0.04) hold += (s[i].x - hold) * 0.5;
        // Keep one sample every ~0.25 s to limit keyframes.
        if (outS.empty() || s[i].time - outS.back().time >= kTicksPerSecond / 4 || i + 1 == s.size())
            outS.push_back({s[i].time, hold, s[i].y});
    }
    return outS;
}

// ---------------------------------------------------------------------------

QString analyzeStabilization(const QString &path, Ticks in, double speed, Ticks duration, FrameRate fps, double smoothness, bool rotation,
                             const Progress &progress, QString *error)
{
    constexpr int W = 320, H = 180;
    struct Est { Ticks t; double dx, dy, da; };
    std::vector<Est> est;
    std::vector<uint8_t> prev;
    const Ticks out = in + Ticks(double(duration) * speed);
    const Ticks span = std::max<Ticks>(1, out - in);
    const int BX = 10, BY = 6, BS = 16, R = 12;
    scanFrames(path, in, out, W, H, true, [&](Ticks t, const uint8_t *d, int stride) {
        std::vector<uint8_t> cur(size_t(W) * H);
        for (int y = 0; y < H; ++y) std::memcpy(&cur[size_t(y) * W], d + y * stride, W);
        if (prev.empty()) {
            est.push_back({t, 0, 0, 0});
        } else {
            std::vector<double> mx, my, px, py;
            for (int by = 0; by < BY; ++by)
                for (int bx = 0; bx < BX; ++bx) {
                    const int x0 = R + (W - 2 * R - BS) * bx / (BX - 1);
                    const int y0 = R + (H - 2 * R - BS) * by / (BY - 1);
                    // Skip flat blocks (no texture to match).
                    int mn = 255, mxv = 0;
                    for (int y = 0; y < BS; ++y) for (int x = 0; x < BS; ++x) { int v = prev[(y0 + y) * W + x0 + x]; mn = std::min(mn, v); mxv = std::max(mxv, v); }
                    if (mxv - mn < 20) continue;
                    int best = INT32_MAX, bdx = 0, bdy = 0;
                    for (int dy = -R; dy <= R; dy += 1)
                        for (int dx = -R; dx <= R; dx += 1) {
                            int sad = 0;
                            for (int y = 0; y < BS && sad < best; y += 2)
                                for (int x = 0; x < BS; x += 2)
                                    sad += std::abs(int(prev[(y0 + y) * W + x0 + x]) - int(cur[(y0 + y + dy) * W + x0 + x + dx]));
                            if (sad < best) { best = sad; bdx = dx; bdy = dy; }
                        }
                    mx.push_back(bdx);
                    my.push_back(bdy);
                    px.push_back(x0 + BS / 2.0 - W / 2.0);
                    py.push_back(y0 + BS / 2.0 - H / 2.0);
                }
            double dx = 0, dy = 0, da = 0;
            if (!mx.empty()) {
                auto median = [](std::vector<double> v) { std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); return v[v.size() / 2]; };
                dx = median(mx);
                dy = median(my);
                if (rotation) {
                    // Least squares rotation from residual motion: v = da x (-py, px).
                    double num = 0, den = 0;
                    for (size_t i = 0; i < mx.size(); ++i) {
                        const double rx = mx[i] - dx, ry = my[i] - dy;
                        if (std::abs(rx) > 6 || std::abs(ry) > 6) continue;   // outliers (moving subjects)
                        num += -py[i] * rx + px[i] * ry;
                        den += px[i] * px[i] + py[i] * py[i];
                    }
                    da = den > 0 ? num / den : 0;
                }
            }
            est.push_back({t, dx / W, dy / H, da});
        }
        prev.swap(cur);
        return !progress || progress(double(t - in) / span);
    }, error);
    if (est.size() < 2)
        return {};
    // Camera trajectory, then smoothed trajectory (Gaussian).
    const size_t n = est.size();
    std::vector<double> tx(n), ty(n), ta(n);
    for (size_t i = 1; i < n; ++i) {
        tx[i] = tx[i - 1] + est[i].dx;
        ty[i] = ty[i - 1] + est[i].dy;
        ta[i] = ta[i - 1] + est[i].da;
    }
    const int radius = std::max(1, int(smoothness / 100.0 * 60));
    const double sigma = radius / 2.5;
    std::vector<double> kern(2 * radius + 1);
    for (int k = -radius; k <= radius; ++k) kern[k + radius] = std::exp(-k * k / (2 * sigma * sigma));
    auto smooth = [&](const std::vector<double> &v) {
        std::vector<double> o(n);
        for (size_t i = 0; i < n; ++i) {
            double s = 0, w = 0;
            for (int k = -radius; k <= radius; ++k) {
                const long j = std::clamp<long>(long(i) + k, 0, long(n) - 1);
                s += v[j] * kern[k + radius];
                w += kern[k + radius];
            }
            o[i] = s / w;
        }
        return o;
    };
    const auto sx = smooth(tx), sy = smooth(ty), sa = smooth(ta);
    // Sample corrections at the sequence frame rate across the clip.
    QJsonArray arr;
    const int64_t frames = std::max<int64_t>(1, ticksToFrame(duration, fps));
    size_t j = 0;
    for (int64_t f = 0; f < frames; ++f) {
        const Ticks src = in + Ticks(double(frameToTicks(f, fps)) * speed);
        while (j + 1 < n && est[j + 1].t <= src) ++j;
        // Shift the image opposite to the jitter.
        arr.append(QJsonArray{sx[j] - tx[j], sy[j] - ty[j], rotation ? (sa[j] - ta[j]) : 0.0, 1.0});
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

// ---------------------------------------------------------------------------

QString whisperHome()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/raccord/whisper";
}

#ifdef Q_OS_WIN
// Windows: an embeddable Python shipped with Raccord ("python" next to the
// executable) is copied to the user data folder on first install.
static QString whisperPython() { return whisperHome() + "/python/python.exe"; }
static QString bundledPython() { return QCoreApplication::applicationDirPath() + "/python"; }
#else
static QString whisperPython() { return whisperHome() + "/venv/bin/python3"; }
#endif

static QString helperScript()
{
    // Written next to the venv so it works from an AppImage too.
    const QString path = whisperHome() + "/transcribe.py";
    static const char *script = R"PY(
import json, sys
from faster_whisper import WhisperModel
audio, model_name, language = sys.argv[1], sys.argv[2], sys.argv[3]
try:
    model = WhisperModel(model_name, device="auto", compute_type="auto")
except Exception:
    model = WhisperModel(model_name, device="cpu", compute_type="int8")
segments, info = model.transcribe(audio, language=None if language == "auto" else language,
                                  word_timestamps=True, vad_filter=True)
total = info.duration or 1
for s in segments:
    words = [{"s": w.start, "e": w.end, "w": w.word} for w in (s.words or [])]
    print(json.dumps({"start": s.start, "end": s.end, "text": s.text.strip(), "words": words, "p": s.end / total}), flush=True)
)PY";
    QFile f(path);
    QDir().mkpath(whisperHome());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(script);
    return path;
}

bool whisperAvailable(QString *detail)
{
    if (!QFile::exists(whisperPython())) {
        if (detail) *detail = QObject::tr("Transcription engine not installed");
        return false;
    }
    QProcess p;
    p.start(whisperPython(), {"-c", "import faster_whisper; print(faster_whisper.__version__)"});
    p.waitForFinished(60000);
    const QString version = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
    const bool ok = p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0 && !version.isEmpty();
    if (detail) *detail = ok ? QObject::tr("faster-whisper %1").arg(version)
                             : QString::fromUtf8(p.readAllStandardError()).trimmed();
    return ok;
}

bool installWhisper(const std::function<void(const QString &)> &log, QString *error)
{
    QDir().mkpath(whisperHome());
    auto run = [&](const QString &prog, const QStringList &args) {
        QProcess p;
        p.setProcessChannelMode(QProcess::MergedChannels);
        p.start(prog, args);
        if (!p.waitForStarted(10000)) {
            if (error) *error = QObject::tr("Cannot start %1").arg(prog);
            return false;
        }
        while (!p.waitForFinished(500))
            if (log) { const QString out = QString::fromUtf8(p.readAll()); if (!out.isEmpty()) log(out); }
        if (log) log(QString::fromUtf8(p.readAll()));
        if (p.exitCode() != 0) {
            if (error) *error = QObject::tr("%1 failed (exit code %2)").arg(prog).arg(p.exitCode());
            return false;
        }
        return true;
    };
#ifdef Q_OS_WIN
    if (!QFile::exists(whisperPython())) {
        const QString src = bundledPython();
        if (!QFile::exists(src + "/python.exe")) {
            if (error) *error = QObject::tr("The Python runtime shipped with Raccord is missing (%1)").arg(src);
            return false;
        }
        // Copy the runtime, then enable site-packages (embeddable builds disable it).
        const QString dst = whisperHome() + "/python";
        QDirIterator it(src, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString f = it.next();
            const QString target = dst + f.mid(src.size());
            QDir().mkpath(QFileInfo(target).absolutePath());
            QFile::remove(target);
            QFile::copy(f, target);
        }
        for (const QFileInfo &pth : QDir(dst).entryInfoList({"python*._pth"}, QDir::Files)) {
            QFile f(pth.absoluteFilePath());
            if (f.open(QIODevice::ReadOnly)) {
                QString text = QString::fromUtf8(f.readAll());
                f.close();
                text.replace("#import site", "import site");
                if (!text.contains("Lib\\site-packages")) text += "\nLib\\site-packages\n";
                if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(text.toUtf8());
            }
        }
        if (log) log(QObject::tr("Installing pip..."));
        if (!run(whisperPython(), {dst + "/get-pip.py", "--no-warn-script-location"}))
            return false;
    }
#else
    const QString py = QStandardPaths::findExecutable("python3");
    if (py.isEmpty()) {
        if (error) *error = QObject::tr("python3 is required to install the transcription engine");
        return false;
    }
    if (!QFile::exists(whisperPython()) && !run(py, {"-m", "venv", whisperHome() + "/venv"}))
        return false;
#endif
    if (!run(whisperPython(), {"-m", "pip", "install", "--upgrade", "pip"}))
        return false;
    return run(whisperPython(), {"-m", "pip", "install", "faster-whisper"});
}

static bool writeWav16k(const QString &src, int stream, Ticks in, Ticks out, const QString &dst, QString *error)
{
    AudioReader r;
    constexpr int rate = 16000;
    if (!r.open(src, stream, rate, error))
        return false;
    QFile f(dst);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    const int ch = r.channels();
    const int64_t s0 = int64_t(ticksToSeconds(in) * rate), s1 = int64_t(ticksToSeconds(out) * rate);
    const uint32_t frames = uint32_t(std::max<int64_t>(0, s1 - s0));
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char *>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char *>(&v), 2); };
    f.write("RIFF"); u32(36 + frames * 2); f.write("WAVEfmt "); u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
    f.write("data"); u32(frames * 2);
    std::vector<float> buf(size_t(rate) * ch);
    std::vector<int16_t> pcm(rate);
    for (int64_t pos = s0; pos < s1; pos += rate) {
        const int n = int(std::min<int64_t>(rate, s1 - pos));
        r.read(pos, n, buf.data());
        for (int i = 0; i < n; ++i) {
            float m = 0;
            for (int c = 0; c < ch; ++c) m += buf[size_t(i) * ch + c];
            pcm[i] = int16_t(std::clamp(m / ch, -1.f, 1.f) * 32767);
        }
        f.write(reinterpret_cast<const char *>(pcm.data()), n * 2);
    }
    return true;
}

std::vector<Segment> transcribe(const QString &path, int audioStream, Ticks in, Ticks out, const QString &model, const QString &language,
                                const Progress &progress, QString *error)
{
    std::vector<Segment> segs;
    QString detail;
    if (!whisperAvailable(&detail)) {
        if (error) *error = detail;
        return segs;
    }
    QTemporaryDir tmp;
    const QString wav = tmp.path() + "/audio.wav";
    if (!writeWav16k(path, audioStream, in, out, wav, error))
        return segs;
    QProcess p;
    p.start(whisperPython(), {helperScript(), wav, model.isEmpty() ? "small" : model, language.isEmpty() ? "auto" : language});
    if (!p.waitForStarted(10000)) {
        if (error) *error = QObject::tr("Cannot start the transcription engine");
        return segs;
    }
    QByteArray pending;
    auto consume = [&] {
        pending += p.readAllStandardOutput();
        int nl;
        while ((nl = pending.indexOf('\n')) >= 0) {
            const QByteArray line = pending.left(nl);
            pending.remove(0, nl + 1);
            const QJsonObject o = QJsonDocument::fromJson(line).object();
            if (o.isEmpty()) continue;
            Segment s;
            s.start = in + secondsToTicks(o["start"].toDouble());
            s.end = in + secondsToTicks(o["end"].toDouble());
            s.text = o["text"].toString();
            for (const auto &w : o["words"].toArray()) {
                const QJsonObject wo = w.toObject();
                s.words.push_back({in + secondsToTicks(wo["s"].toDouble()), in + secondsToTicks(wo["e"].toDouble()), wo["w"].toString().trimmed()});
            }
            segs.push_back(s);
            if (progress && !progress(o["p"].toDouble())) {
                p.kill();
                return false;
            }
        }
        return true;
    };
    while (!p.waitForFinished(300))
        if (!consume()) return {};
    consume();
    if (p.exitCode() != 0) {
        if (error) *error = QString::fromUtf8(p.readAllStandardError()).trimmed().section('\n', -3);
        return {};
    }
    return segs;
}

std::vector<Caption> segmentsToCaptions(const std::vector<Segment> &segs, int maxChars, int maxLines, Ticks maxDuration)
{
    std::vector<Caption> out;
    const int limit = std::max(8, maxChars) * std::max(1, maxLines);
    for (const auto &s : segs) {
        std::vector<Word> words = s.words;
        if (words.empty()) {
            // No word timing: spread words evenly.
            const QStringList ws = s.text.split(' ', Qt::SkipEmptyParts);
            for (int i = 0; i < ws.size(); ++i) {
                const Ticks a = s.start + (s.end - s.start) * i / std::max<qsizetype>(1, ws.size());
                const Ticks b = s.start + (s.end - s.start) * (i + 1) / std::max<qsizetype>(1, ws.size());
                words.push_back({a, b, ws[i]});
            }
        }
        Caption cur;
        auto flush = [&] {
            if (cur.text.isEmpty()) return;
            cur.id = newId();
            // Line breaks for readability.
            if (cur.text.size() > maxChars && maxLines > 1) {
                int mid = cur.text.size() / 2;
                int sp = cur.text.lastIndexOf(' ', mid);
                int sp2 = cur.text.indexOf(' ', mid);
                int cut = (sp < 0) ? sp2 : (sp2 < 0 ? sp : (mid - sp <= sp2 - mid ? sp : sp2));
                if (cut > 0) cur.text[cut] = '\n';
            }
            out.push_back(cur);
            cur = Caption();
        };
        for (const auto &w : words) {
            const bool sentenceEnd = !cur.text.isEmpty() && QString(".!?").contains(cur.text.back()) && cur.text.size() > maxChars / 2;
            if (!cur.text.isEmpty() && (cur.text.size() + 1 + w.text.size() > limit || w.end - cur.start > maxDuration || sentenceEnd))
                flush();
            if (cur.text.isEmpty()) {
                cur.start = w.start;
                cur.text = w.text;
            } else {
                cur.text += " " + w.text;
            }
            cur.end = w.end;
        }
        flush();
    }
    // Avoid overlaps and flicker: extend short gaps.
    for (size_t i = 0; i + 1 < out.size(); ++i) {
        if (out[i].end > out[i + 1].start) out[i].end = out[i + 1].start;
        if (out[i + 1].start - out[i].end < kTicksPerSecond / 4) out[i].end = out[i + 1].start;
    }
    return out;
}

} // namespace rc::ai
