// raccord-cli: headless tools (probe, render a frame, export, build a project).
#include "core/Document.h"
#include "core/Edit.h"
#include "export/Exporter.h"
#include "media/MediaPool.h"
#include "media/MediaProbe.h"
#include "render/Renderer.h"
#include "render/Title.h"
#include "Version.h"
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QOffscreenSurface>
#include <cstdio>

using namespace rc;

static int usage()
{
    std::fprintf(stderr,
                 "raccord-cli " RACCORD_VERSION "\n"
                 "  probe <media>                                     print media information (JSON)\n"
                 "  presets                                           list export presets\n"
                 "  frame <project> <seconds> <out.png> [width]       render one frame\n"
                 "  export <project> <out> [preset-id] [sequence]     export a sequence\n"
                 "  assemble <out.raccord> <media...>                 build a project with the media end to end\n");
    return 2;
}

static void progress(const ExportProgress &p)
{
    std::fprintf(stderr, "\r%5.1f%%  frame %lld/%lld  %.1f fps  ETA %.0fs   ", p.fraction * 100, (long long)p.frame,
                 (long long)p.totalFrames, p.fps, p.etaSeconds);
}

static Clip makeClip(const MediaItem &m, TrackType type, Ticks pos, Ticks dur, const QString &link)
{
    Clip c;
    c.id = newId();
    c.mediaId = m.id;
    c.name = m.name;
    c.start = pos;
    c.duration = dur;
    c.linkId = link;
    c.ensureIntrinsics(type);
    return c;
}

int main(int argc, char **argv)
{
    // Headless servers: no display, use the offscreen platform (EGL/GLX pbuffers).
    if (qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QStringList args = app.arguments().mid(1);
    if (args.isEmpty())
        return usage();
    const QString cmd = args.takeFirst();

    if (cmd == "probe" && args.size() >= 1) {
        MediaInfo info = probeMedia(args[0]);
        std::printf("%s\n", QJsonDocument(info.toJson()).toJson().constData());
        return info.valid ? 0 : 1;
    }
    if (cmd == "effect-strings") {
        // All user visible strings of the effect registry (for translators).
        QStringList out;
        for (EffectKind k : {EffectKind::Video, EffectKind::Audio, EffectKind::VideoTransition, EffectKind::AudioTransition})
            for (const EffectDef *d : EffectRegistry::instance().all(k)) {
                out << d->name << d->category;
                for (const auto &p : d->params) { out << p.name << p.group; out << p.options; }
            }
        for (const QString &id : {"motion", "opacity", "timeRemap", "volume", "panner"})
            if (const EffectDef *d = EffectRegistry::instance().find(id)) {
                out << d->name;
                for (const auto &p : d->params) { out << p.name << p.group; out << p.options; }
            }
        out.removeDuplicates();
        out.removeAll(QString());
        for (const auto &s : out) std::printf("%s\n", qPrintable(s));
        return 0;
    }
    if (cmd == "presets") {
        for (const auto &p : exportPresets())
            std::printf("%-20s %-10s %s%s\n", qPrintable(p.id), qPrintable(p.category), qPrintable(p.name),
                        encoderAvailable(p.settings.videoCodec) ? "" : "  (encoder unavailable)");
        return 0;
    }
    if (cmd == "assemble" && args.size() >= 2) {
        Document doc;
        const QString out = args.takeFirst();
        doc.edit("assemble", [&](Project &p) {
            Sequence &seq = *p.activeSequence();
            bool first = true;
            Ticks pos = 0;
            QString prevV;
            for (const auto &path : args) {
                MediaItem m;
                m.id = newId();
                m.path = QFileInfo(path).absoluteFilePath();
                m.name = QFileInfo(path).fileName();
                m.info = probeMedia(m.path);
                if (!m.info.valid) {
                    std::fprintf(stderr, "skip %s: %s\n", qPrintable(path), qPrintable(m.info.error));
                    continue;
                }
                if (first && m.info.hasVideo && !m.info.isStill) {
                    seq.width = m.info.rotation % 180 ? m.info.height : m.info.width;
                    seq.height = m.info.rotation % 180 ? m.info.width : m.info.height;
                    seq.fps = m.info.fps;
                    first = false;
                }
                p.media.push_back(m);
                Ticks dur = m.info.isStill ? p.settings.defaultStillDuration : floorToFrame(m.info.duration, seq.fps);
                const QString link = newId();
                std::vector<std::pair<edit::TrackRef, Clip>> list;
                if (m.info.hasVideo)
                    list.push_back({{TrackType::Video, 0}, makeClip(m, TrackType::Video, pos, dur, link)});
                if (m.info.hasAudio)
                    list.push_back({{TrackType::Audio, 0}, makeClip(m, TrackType::Audio, pos, dur, link)});
                edit::overwriteEdit(seq, list);
                if (!prevV.isEmpty() && m.info.hasVideo)
                    edit::addTransition(seq, {TrackType::Video, 0}, prevV, list[0].second.id, "crossDissolve", seq.fps.frameTicks() * 12);
                if (m.info.hasVideo)
                    prevV = list[0].second.id;
                pos += dur;
            }
            Clip t;
            t.id = newId();
            t.source = ClipSource::Title;
            t.name = "Title";
            t.start = 0;
            t.duration = 3 * kTicksPerSecond;
            t.gen = TitleRenderer::templateDoc("Lower Third (Bar)");
            t.ensureIntrinsics(TrackType::Video);
            edit::overwriteEdit(seq, {{{TrackType::Video, 1}, t}});
            return true;
        });
        QString err;
        if (!doc.save(out, &err)) {
            std::fprintf(stderr, "save failed: %s\n", qPrintable(err));
            return 1;
        }
        std::printf("%s\n", qPrintable(doc.path()));
        return 0;
    }

    if ((cmd == "frame" && args.size() >= 3) || (cmd == "export" && args.size() >= 2)) {
        Document doc;
        QString err;
        if (!doc.load(args[0], &err)) {
            std::fprintf(stderr, "load failed: %s\n", qPrintable(err));
            return 1;
        }
        const Project &p = doc.project();
        const Sequence *seq = p.sequence(p.activeSequenceId);
        if (!seq && !p.sequences.empty())
            seq = &p.sequences.front();
        if (cmd == "export" && args.size() >= 4)
            for (const auto &s : p.sequences)
                if (s.name == args[3])
                    seq = &s;
        if (!seq) {
            std::fprintf(stderr, "no sequence\n");
            return 1;
        }
        MediaPool::instance().setCacheDir(doc.cacheDir());
        QOffscreenSurface *surface = OffscreenGL::createSurface();
        if (cmd == "frame") {
            OffscreenGL gl;
            if (!gl.create(surface, &err)) {
                std::fprintf(stderr, "%s\n", qPrintable(err));
                return 1;
            }
            Renderer r;
            if (!r.ok()) {
                std::fprintf(stderr, "renderer: %s\n", qPrintable(r.error()));
                return 1;
            }
            RenderOptions o;
            o.width = args.size() >= 4 ? args[3].toInt() : seq->width;
            o.height = int(int64_t(o.width) * seq->height / seq->width);
            o.exporting = true;
            SurfaceRef s = r.renderSequence(p, *seq, secondsToTicks(args[1].toDouble()), o);
            QImage img = r.gl().readback(s);
            img.save(args[2]);
            std::printf("%s %dx%d\n", qPrintable(args[2]), img.width(), img.height());
            return 0;
        }
        ExportSettings s;
        if (args.size() >= 3) {
            const ExportPreset *pr = findPreset(args[2]);
            if (!pr) {
                std::fprintf(stderr, "unknown preset %s\n", qPrintable(args[2]));
                return 1;
            }
            s = pr->settings;
        } else {
            s = findPreset("match-source")->settings;
        }
        s.path = args[1];
        bool ok = Exporter::run(p, seq->id, s, surface, [](const ExportProgress &pr) { progress(pr); return true; }, &err);
        std::fprintf(stderr, "\n");
        if (!ok) {
            std::fprintf(stderr, "export failed: %s\n", qPrintable(err));
            return 1;
        }
        std::printf("%s\n", qPrintable(s.path));
        return 0;
    }
    return usage();
}
