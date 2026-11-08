#include <catch2/catch.hpp>
#include "ai/Analysis.h"
#include "core/Edit.h"
#include "export/Proxy.h"
#include "interop/Interchange.h"
#include "media/MediaProbe.h"
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>

using namespace rc;

static QString testMedia(const QString &name)
{
    const QString p = QStringLiteral(RACCORD_SOURCE_DIR "/test-media/") + name;
    return QFileInfo::exists(p) ? p : QString();
}

// A project with two linked A/V clips, a cross dissolve, a speed change and a marker.
static Project sampleProject(const QString &mediaPath)
{
    Project p;
    MediaItem m;
    m.id = newId();
    m.name = "clip.mp4";
    m.path = mediaPath;
    m.info = probeMedia(mediaPath);
    if (!m.info.valid) {
        m.info.valid = true;
        m.info.hasVideo = m.info.hasAudio = true;
        m.info.duration = 10 * kTicksPerSecond;
        m.info.width = 1920;
        m.info.height = 1080;
    }
    p.media.push_back(m);
    Sequence s;
    s.id = newId();
    s.name = "Edit";
    s.fps = {25, 1};
    s.ensureDefaultTracks();
    auto add = [&](Ticks start, Ticks dur, Ticks in, double speed) {
        const QString link = newId();
        for (TrackType t : {TrackType::Video, TrackType::Audio}) {
            Clip c;
            c.id = newId();
            c.name = "clip";
            c.mediaId = m.id;
            c.start = start;
            c.duration = dur;
            c.in = in;
            c.speed = speed;
            c.linkId = link;
            c.ensureIntrinsics(t);
            s.tracks(t)[0].clips.push_back(c);
        }
    };
    const Ticks f = s.fps.frameTicks();
    add(0, 50 * f, 0, 1.0);
    add(50 * f, 50 * f, 25 * f, 0.5);
    edit::addTransition(s, {TrackType::Video, 0}, s.videoTracks[0].clips[0].id, s.videoTracks[0].clips[1].id, "crossDissolve", 10 * f);
    Marker mk;
    mk.id = newId();
    mk.name = "chapter";
    mk.time = 30 * f;
    s.markers.push_back(mk);
    p.sequences.push_back(s);
    p.activeSequenceId = s.id;
    return p;
}

static void checkRoundTrip(const Sequence &a, const Sequence &b, bool speed, bool markers)
{
    REQUIRE(b.videoTracks.size() >= 1);
    REQUIRE(b.videoTracks[0].clips.size() == a.videoTracks[0].clips.size());
    for (size_t i = 0; i < a.videoTracks[0].clips.size(); ++i) {
        const Clip &x = a.videoTracks[0].clips[i], &y = b.videoTracks[0].clips[i];
        CHECK(y.start == x.start);
        CHECK(y.duration == x.duration);
        if (speed) {
            CHECK(std::abs(y.speed - x.speed) < 1e-6);
            CHECK(std::llabs(y.in - x.in) <= a.fps.frameTicks());
        }
    }
    if (markers) CHECK(b.markers.size() == a.markers.size());
}

TEST_CASE("FCP7 XML round trip (Premiere Pro interchange)")
{
    QTemporaryDir dir;
    const QString media = testMedia("a_1080p.mp4").isEmpty() ? dir.path() + "/missing.mp4" : testMedia("a_1080p.mp4");
    Project p = sampleProject(media);
    const QString xml = dir.path() + "/edit.xml";
    QString err;
    REQUIRE(interop::exportFcp7Xml(p, p.sequences[0], xml, &err));
    Project q;
    QStringList ids = interop::importFcp7Xml(xml, q, &err);
    INFO(err.toStdString());
    REQUIRE(ids.size() == 1);
    const Sequence &s = *q.sequence(ids[0]);
    checkRoundTrip(p.sequences[0], s, true, true);
    CHECK(s.audioTracks[0].clips.size() == 2);
    CHECK(s.videoTracks[0].transitions.size() == 1);
    // A/V links survive.
    CHECK(!s.videoTracks[0].clips[0].linkId.isEmpty());
    CHECK(s.videoTracks[0].clips[0].linkId == s.audioTracks[0].clips[0].linkId);
    CHECK(q.media.size() == 1);
}

TEST_CASE("OpenTimelineIO round trip")
{
    QTemporaryDir dir;
    const QString media = testMedia("a_1080p.mp4").isEmpty() ? dir.path() + "/missing.mp4" : testMedia("a_1080p.mp4");
    Project p = sampleProject(media);
    const QString f = dir.path() + "/edit.otio";
    QString err;
    REQUIRE(interop::exportOtio(p, p.sequences[0], f, &err));
    Project q;
    QStringList ids = interop::importOtio(f, q, &err);
    REQUIRE(ids.size() == 1);
    const Sequence &s = *q.sequence(ids[0]);
    checkRoundTrip(p.sequences[0], s, true, true);
    CHECK(s.videoTracks[0].transitions.size() == 1);
}

TEST_CASE("EDL export and import")
{
    QTemporaryDir dir;
    const QString media = testMedia("a_1080p.mp4").isEmpty() ? dir.path() + "/missing.mp4" : testMedia("a_1080p.mp4");
    Project p = sampleProject(media);
    // EDL keeps cuts but not exact speed for the dissolve source; compare positions only.
    const QString f = dir.path() + "/edit.edl";
    QString err;
    REQUIRE(interop::exportEdl(p, p.sequences[0], f, &err));
    Project q;
    QStringList ids = interop::importEdl(f, q, {25, 1}, &err);
    REQUIRE(ids.size() == 1);
    const Sequence &s = *q.sequence(ids[0]);
    REQUIRE(s.videoTracks[0].clips.size() == 2);
    CHECK(s.videoTracks[0].clips[0].start == 0);
    CHECK(s.videoTracks[0].clips[1].start == p.sequences[0].videoTracks[0].clips[1].start);
}

TEST_CASE("FCPXML export writes a valid document")
{
    QTemporaryDir dir;
    Project p = sampleProject(dir.path() + "/missing.mp4");
    QString err;
    REQUIRE(interop::exportFcpxml(p, p.sequences[0], dir.path() + "/edit.fcpxml", &err));
    QFile f(dir.path() + "/edit.fcpxml");
    REQUIRE(f.open(QIODevice::ReadOnly));
    const QByteArray d = f.readAll();
    CHECK(d.contains("<fcpxml version=\"1.9\""));
    CHECK(d.count("<asset-clip") == 4);
}

TEST_CASE("silence detection finds the gaps")
{
    const QString f = testMedia("e_silence.mp4");
    if (f.isEmpty()) return;
    // The test file is silent from 2-3 s and 6-7 s.
    QString err;
    auto r = ai::detectSilence(f, 0, 0, 8 * kTicksPerSecond, -40, kTicksPerSecond / 2, 0, {}, &err);
    INFO(err.toStdString());
    REQUIRE(r.size() == 2);
    CHECK(std::abs(ticksToSeconds(r[0].start) - 2.0) < 0.15);
    CHECK(std::abs(ticksToSeconds(r[0].end) - 3.0) < 0.15);
    CHECK(std::abs(ticksToSeconds(r[1].start) - 6.0) < 0.15);
}

TEST_CASE("scene detection on an edited file")
{
    const QString f = QStringLiteral(RACCORD_SOURCE_DIR "/test-media/scenes.mp4");
    if (!QFileInfo::exists(f)) return;
    // scenes.mp4: testsrc 0-2 s, smptebars 2-4 s, mandelbrot 4-6 s.
    auto cuts = ai::detectScenes(f, 0, 6 * kTicksPerSecond, 0.5);
    REQUIRE(cuts.size() == 2);
    CHECK(std::abs(ticksToSeconds(cuts[0]) - 2.0) < 0.1);
    CHECK(std::abs(ticksToSeconds(cuts[1]) - 4.0) < 0.1);
}

TEST_CASE("proxy generation")
{
    const QString f = testMedia("c_rot.mp4");
    if (f.isEmpty()) return;
    QTemporaryDir dir;
    MediaItem m;
    m.path = f;
    m.info = probeMedia(f);
    const QString out = proxyPathFor(dir.path(), m, 360);
    QString err;
    REQUIRE(makeProxy(m, out, 360, {}, &err));
    MediaInfo pi = probeMedia(out);
    REQUIRE(pi.valid);
    // Rotated 1280x720 source: proxy is portrait, rotation baked in.
    CHECK(pi.rotation == 0);
    CHECK(pi.height == 360);
    CHECK(pi.width == 202);
    CHECK(std::abs(ticksToSeconds(pi.duration) - ticksToSeconds(m.info.duration)) < 0.1);
}
