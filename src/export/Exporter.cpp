#include "Exporter.h"
#include "Encoder.h"
#include "audio/AudioMixer.h"
#include "interop/Subtitles.h"
#include "render/Renderer.h"
#include <QElapsedTimer>
#include <QFileInfo>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#ifdef RACCORD_HAVE_EBUR128
#include <ebur128.h>
#endif

namespace rc {

QOffscreenSurface *OffscreenGL::createSurface()
{
    auto *s = new QOffscreenSurface;
    QSurfaceFormat f;
    f.setVersion(3, 3);
    f.setProfile(QSurfaceFormat::CoreProfile);
    s->setFormat(f);
    s->create();
    return s;
}

bool OffscreenGL::create(QOffscreenSurface *s, QString *error)
{
    surface = s;
    context = new QOpenGLContext;
    QSurfaceFormat f;
    f.setVersion(3, 3);
    f.setProfile(QSurfaceFormat::CoreProfile);
    context->setFormat(f);
    if (QOpenGLContext *g = QOpenGLContext::globalShareContext())
        context->setShareContext(g);
    if (!context->create() || !context->makeCurrent(surface)) {
        if (error) *error = "Cannot create an OpenGL 3.3 context (GPU driver or display missing)";
        delete context;
        context = nullptr;
        return false;
    }
    return true;
}

void OffscreenGL::destroy()
{
    if (context) {
        context->doneCurrent();
        delete context;
        context = nullptr;
    }
}

double Exporter::measureLoudness(const Project &project, const Sequence &seq, TimeRange range, double *truePeakDb)
{
#ifdef RACCORD_HAVE_EBUR128
    const int ch = std::max(1, seq.audioChannels);
    ebur128_state *st = ebur128_init(ch, seq.sampleRate, EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK);
    if (!st)
        return 0;
    AudioMixer mixer;
    const int block = 4096;
    std::vector<float> buf(size_t(block) * ch);
    int64_t s0 = ticksToSamples(range.start, seq.sampleRate), s1 = ticksToSamples(range.end, seq.sampleRate);
    for (int64_t p = s0; p < s1; p += block) {
        int n = int(std::min<int64_t>(block, s1 - p));
        mixer.mix(project, seq, p, n, buf.data());
        ebur128_add_frames_float(st, buf.data(), n);
    }
    double lufs = 0;
    ebur128_loudness_global(st, &lufs);
    if (truePeakDb) {
        double peak = 0;
        for (int c = 0; c < ch; ++c) {
            double p = 0;
            ebur128_true_peak(st, c, &p);
            peak = std::max(peak, p);
        }
        *truePeakDb = peak > 0 ? 20 * std::log10(peak) : -120;
    }
    ebur128_destroy(&st);
    return std::isfinite(lufs) ? lufs : -70;
#else
    (void)project; (void)seq; (void)range; (void)truePeakDb;
    return 0;
#endif
}

bool Exporter::run(const Project &project, const QString &sequenceId, const ExportSettings &sIn, QOffscreenSurface *surface,
                   const ExportCallback &cb, QString *error)
{
    const Sequence *seq = project.sequence(sequenceId);
    if (!seq) {
        if (error) *error = "Sequence not found";
        return false;
    }
    ExportSettings s = sIn;
    const int W = (s.width > 0 ? s.width : seq->width) & ~1;
    const int H = (s.height > 0 ? s.height : seq->height) & ~1;
    const FrameRate fps = s.fps.valid() ? s.fps : seq->fps;
    const int rate = s.sampleRate > 0 ? s.sampleRate : seq->sampleRate;
    TimeRange range = s.useWorkArea ? seq->workRange() : TimeRange{0, seq->duration()};
    if (s.rangeStart != kInvalidTicks) range.start = s.rangeStart;
    if (s.rangeEnd != kInvalidTicks) range.end = s.rangeEnd;
    if (range.empty()) {
        if (error) *error = "Nothing to export (empty sequence or range)";
        return false;
    }
    // The mixer works at the sequence rate; the encoder resamples if needed.
    Sequence mixSeq = *seq;
    mixSeq.audioChannels = s.channels;

    // Loudness normalisation: measure first, then apply a master gain.
    if (s.loudnessTarget < 0 && s.audioCodec != "none") {
        double tp = 0;
        double lufs = measureLoudness(project, mixSeq, range, &tp);
        if (lufs > -69) {
            double gain = s.loudnessTarget - lufs;
            // Respect the true peak ceiling.
            if (tp + gain > s.truePeakDb)
                gain = s.truePeakDb - tp;
            mixSeq.masterVolumeDb += gain;
        }
    }

    Encoder enc;
    if (!enc.open(s, W, H, fps, mixSeq.sampleRate, s.channels, error))
        return false;
    (void)rate;

    OffscreenGL gl;
    std::unique_ptr<Renderer> renderer;
    if (enc.hasVideo()) {
        if (!gl.create(surface, error))
            return false;
        renderer = std::make_unique<Renderer>();
        if (!renderer->ok()) {
            if (error) *error = renderer->error();
            return false;
        }
    }
    RenderOptions ro;
    ro.width = W;
    ro.height = H;
    ro.exporting = true;
    ro.useProxies = s.useProxies;
    ro.keepAlpha = s.alpha;
    ro.showCaptions = s.burnCaptions;
    ro.showHslMask = false;
    Project proj = project;
    if (s.burnCaptions)
        for (auto &sq : proj.sequences)
            if (sq.id == seq->id)
                for (auto &ct : sq.captionTracks)
                    ct.burnIn = ct.burnIn || ct.visible;
    const Sequence *rseq = proj.sequence(seq->id);

    const int64_t f0 = ticksToFrame(range.start + fps.frameTicks() / 2, fps);
    const int64_t f1 = ticksToFrame(range.end + fps.frameTicks() / 2, fps);
    const int64_t total = std::max<int64_t>(1, f1 - f0);
    AudioMixer mixer;
    std::vector<uint16_t> pix;
    std::vector<float> audio;
    int64_t audioPos = ticksToSamples(range.start, mixSeq.sampleRate);
    const int64_t audioEnd = ticksToSamples(range.end, mixSeq.sampleRate);
    QElapsedTimer timer;
    timer.start();
    ExportProgress prog;
    prog.totalFrames = total;
    bool ok = true;
    const int64_t loops = enc.hasVideo() ? total : (audioEnd - audioPos + 47999) / 48000;
    for (int64_t i = 0; i < loops; ++i) {
        if (enc.hasVideo()) {
            const Ticks t = frameToTicks(f0 + i, fps);
            SurfaceRef out = renderer->renderSequence(proj, *rseq, t, ro);
            if (!out) {
                if (error) *error = "Render failed";
                ok = false;
                break;
            }
            renderer->gl().readback16(out, pix);
            if (s.alpha) {
                // Premultiplied -> straight.
                for (size_t k = 0; k < pix.size(); k += 4) {
                    uint32_t a = pix[k + 3];
                    if (a > 0 && a < 65535)
                        for (int c = 0; c < 3; ++c)
                            pix[k + c] = uint16_t(std::min<uint32_t>(65535, pix[k + c] * 65535u / a));
                }
            }
            if (!enc.writeVideo(pix.data(), i)) {
                if (error) *error = "Video encoding failed";
                ok = false;
                break;
            }
            if (cb && (i % 30 == 0))
                prog.preview = renderer->gl().readback(renderer->downscale(out, 320));
            else
                prog.preview = QImage();
        }
        if (enc.hasAudio()) {
            // Audio up to the end of this frame.
            int64_t target = enc.hasVideo() ? std::min(audioEnd, ticksToSamples(frameToTicks(f0 + i + 1, fps), mixSeq.sampleRate))
                                            : std::min(audioEnd, audioPos + 48000);
            while (audioPos < target) {
                int n = int(std::min<int64_t>(4096, target - audioPos));
                audio.resize(size_t(n) * s.channels);
                mixer.mix(proj, mixSeq, audioPos, n, audio.data());
                for (auto &v : audio)
                    v = std::clamp(v, -1.0f, 1.0f);
                if (!enc.writeAudio(audio.data(), n)) {
                    if (error) *error = "Audio encoding failed";
                    ok = false;
                    break;
                }
                audioPos += n;
            }
            if (!ok)
                break;
        }
        prog.frame = i + 1;
        prog.fraction = double(i + 1) / double(loops);
        const double el = timer.elapsed() / 1000.0;
        prog.fps = el > 0 ? (i + 1) / el : 0;
        prog.etaSeconds = prog.fps > 0 ? (loops - i - 1) / prog.fps : 0;
        if (cb && !cb(prog)) {
            if (error) *error = "Cancelled";
            ok = false;
            break;
        }
    }
    QString finishErr;
    bool fin = enc.finish(&finishErr);
    if (renderer) {
        renderer.reset();
        gl.destroy();
    }
    if (ok && !fin) {
        if (error) *error = "Finalising file failed: " + finishErr;
        return false;
    }
    if (ok && s.sidecarSrt && !rseq->captionTracks.empty()) {
        QString srt = QFileInfo(s.path).absolutePath() + "/" + QFileInfo(s.path).completeBaseName() + ".srt";
        writeSrt(rseq->captionTracks.front(), srt, range.start);
    }
    return ok;
}

} // namespace rc
