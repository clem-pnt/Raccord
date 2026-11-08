#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include "core/Time.h"
#include "core/Param.h"
#include "core/Model.h"
#include "core/Edit.h"
#include "core/Document.h"
#include <QJsonDocument>
#include <QCoreApplication>

using namespace rc;

TEST_CASE("frame rates are exact in ticks")
{
    for (FrameRate r : {FrameRate{24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1}, {50, 1}, {60000, 1001}, {60, 1}, {120, 1}}) {
        REQUIRE(kTicksPerSecond * r.den % r.num == 0);
        REQUIRE(ticksToFrame(frameToTicks(123456, r), r) == 123456);
    }
    REQUIRE(kTicksPerSecond % 48000 == 0);
    REQUIRE(kTicksPerSecond % 44100 == 0);
}

TEST_CASE("timecode formatting and parsing")
{
    FrameRate r25{25, 1};
    REQUIRE(formatTimecode(frameToTicks(25 * 3600 + 25 * 61 + 3, r25), r25) == "01:01:01:03");
    REQUIRE(parseTimecode("01:01:01:03", r25) == frameToTicks(25 * 3600 + 25 * 61 + 3, r25));
    REQUIRE(parseTimecode("1000", r25) == frameToTicks(250, r25));
    REQUIRE(parseTimecode("+10", r25, frameToTicks(5, r25)) == frameToTicks(15, r25));
    FrameRate ntsc{30000, 1001};
    // Drop frame: frame 1800 = 00:01:00;02
    REQUIRE(formatTimecode(frameToTicks(1800, ntsc), ntsc) == "00:01:00;02");
    REQUIRE(parseTimecode("00:01:00;02", ntsc) == frameToTicks(1800, ntsc));
    REQUIRE(formatTimecode(frameToTicks(17982, ntsc), ntsc) == "00:10:00;00");
}

TEST_CASE("keyframe interpolation")
{
    AnimatedParam p(Value(0.0));
    p.addKey({0, Value(0.0), Interp::Linear});
    p.addKey({100, Value(10.0), Interp::Hold});
    p.addKey({200, Value(20.0), Interp::Linear});
    REQUIRE(p.valueAt(-5).f() == Approx(0));
    REQUIRE(p.valueAt(50).f() == Approx(5));
    REQUIRE(p.valueAt(150).f() == Approx(10));   // hold
    REQUIRE(p.valueAt(300).f() == Approx(20));
    auto j = p.toJson();
    AnimatedParam q = AnimatedParam::fromJson(j);
    REQUIRE(q.valueAt(50).f() == Approx(5));
    REQUIRE(q.keys().size() == 3);
}

static Clip mk(Ticks start, Ticks dur, Ticks in = 0)
{
    Clip c;
    c.id = newId();
    c.mediaId = "m";
    c.start = start;
    c.duration = dur;
    c.in = in;
    c.ensureIntrinsics(TrackType::Video);
    return c;
}

static Sequence seq3()
{
    Sequence s;
    s.id = newId();
    s.ensureDefaultTracks();
    return s;
}

TEST_CASE("overwrite and insert")
{
    Sequence s = seq3();
    edit::TrackRef v1{TrackType::Video, 0};
    REQUIRE(edit::overwriteEdit(s, {{v1, mk(0, 100)}}));
    REQUIRE(edit::overwriteEdit(s, {{v1, mk(100, 100)}}));
    // Overwrite in the middle splits the first clip.
    REQUIRE(edit::overwriteEdit(s, {{v1, mk(50, 20)}}));
    auto &t = s.videoTracks[0];
    REQUIRE(t.clips.size() == 4);
    REQUIRE(t.clips[0].duration == 50);
    REQUIRE(t.clips[1].start == 50);
    REQUIRE(t.clips[2].start == 70);
    REQUIRE(t.clips[2].in == 70);
    // Insert shifts everything.
    REQUIRE(edit::insertEdit(s, {{v1, mk(60, 10)}}, 60, 10));
    REQUIRE(t.endTime() == 210);
}

TEST_CASE("razor, ripple delete, extract")
{
    Sequence s = seq3();
    edit::TrackRef v1{TrackType::Video, 0};
    edit::overwriteEdit(s, {{v1, mk(0, 100)}});
    edit::overwriteEdit(s, {{v1, mk(100, 100)}});
    REQUIRE(edit::razor(s, 30) == 1);
    auto &t = s.videoTracks[0];
    REQUIRE(t.clips.size() == 3);
    REQUIRE(t.clips[1].in == 30);
    edit::rippleDeleteClips(s, {t.clips[1].id});
    REQUIRE(t.clips.size() == 2);
    REQUIRE(t.clips[1].start == 30);
    edit::extract(s, {10, 20}, {v1});
    REQUIRE(t.endTime() == 120);
}

TEST_CASE("trim, roll, slip, slide")
{
    Sequence s = seq3();
    edit::TrackRef v1{TrackType::Video, 0};
    Clip a = mk(0, 100, 50), b = mk(100, 100, 50), c = mk(200, 100, 50);
    edit::overwriteEdit(s, {{v1, a}, {v1, b}, {v1, c}});
    auto len = [](const Clip &) -> Ticks { return 1000; };
    // Normal trim cannot overlap the neighbour.
    REQUIRE_FALSE(edit::trimOut(s, a.id, 110, edit::TrimMode::Normal, len));
    REQUIRE(edit::trimOut(s, a.id, 90, edit::TrimMode::Normal, len));
    // Ripple trim pushes the rest.
    REQUIRE(edit::trimOut(s, a.id, 120, edit::TrimMode::Ripple, len));
    REQUIRE(s.clip(b.id)->start == 130);   // the 10 tick gap is preserved
    REQUIRE(s.clip(c.id)->start == 230);
    // Roll between b and c.
    REQUIRE(edit::roll(s, b.id, c.id, 210, len));
    REQUIRE(s.clip(b.id)->duration == 80);
    REQUIRE(s.clip(c.id)->start == 210);
    REQUIRE(s.clip(c.id)->in == 30);
    // Source limit on head trim.
    REQUIRE_FALSE(edit::trimIn(s, a.id, -60, edit::TrimMode::Normal, len));
    REQUIRE(edit::slip(s, b.id, 10, len));
    REQUIRE(s.clip(b.id)->in == 60);
    REQUIRE(edit::slide(s, b.id, 10, len));
    REQUIRE(s.clip(b.id)->start == 140);
    REQUIRE(s.clip(a.id)->end() == 120);   // not adjacent: untouched
    REQUIRE(s.clip(c.id)->start == 220);
    REQUIRE(s.clip(c.id)->in == 40);
}

TEST_CASE("transitions survive and are cleaned up")
{
    Sequence s = seq3();
    edit::TrackRef v1{TrackType::Video, 0};
    Clip a = mk(0, 100), b = mk(100, 100);
    edit::overwriteEdit(s, {{v1, a}, {v1, b}});
    QString tid = edit::addTransition(s, v1, a.id, b.id, "crossDissolve", 40);
    REQUIRE(!tid.isEmpty());
    auto r = edit::transitionRange(s.videoTracks[0], *s.videoTracks[0].transition(tid));
    REQUIRE(r.start == 80);
    REQUIRE(r.end == 120);
    edit::moveClips(s, {b.id}, 50, 0, 0);
    REQUIRE(s.videoTracks[0].transitions.empty());
}

TEST_CASE("move clips across tracks and linked selection")
{
    Sequence s = seq3();
    Clip v = mk(0, 100);
    Clip a = mk(0, 100);
    a.effects.clear();
    a.ensureIntrinsics(TrackType::Audio);
    v.linkId = a.linkId = "L";
    edit::overwriteEdit(s, {{{TrackType::Video, 0}, v}, {{TrackType::Audio, 0}, a}});
    REQUIRE(s.linkedClips(v.id).size() == 2);
    REQUIRE(edit::moveClips(s, s.linkedClips(v.id), 25, 1, 1));
    REQUIRE(s.videoTracks[1].clips.size() == 1);
    REQUIRE(s.audioTracks[1].clips.size() == 1);
    REQUIRE(s.videoTracks[1].clips[0].start == 25);
    // Razor keeps link groups separated.
    edit::razor(s, 50);
    REQUIRE(s.linkedClips(s.videoTracks[1].clips[1].id).size() == 2);
    REQUIRE(s.videoTracks[1].clips[0].linkId != s.videoTracks[1].clips[1].linkId);
}

TEST_CASE("nesting")
{
    Project p;
    Sequence s = seq3();
    edit::TrackRef v1{TrackType::Video, 0};
    Clip a = mk(100, 100), b = mk(200, 50);
    edit::overwriteEdit(s, {{v1, a}, {v1, b}});
    p.sequences.push_back(s);
    QString nid = edit::nest(p, p.sequences[0], {a.id, b.id}, "Nest");
    REQUIRE(!nid.isEmpty());
    REQUIRE(p.sequences.size() == 2);
    REQUIRE(p.sequences[0].videoTracks[0].clips.size() == 1);
    REQUIRE(p.sequences[0].videoTracks[0].clips[0].duration == 150);
    REQUIRE(p.sequence(nid)->videoTracks[0].clips[0].start == 0);
}

TEST_CASE("speed change")
{
    Sequence s = seq3();
    edit::TrackRef v1{TrackType::Video, 0};
    Clip a = mk(0, 100), b = mk(100, 100);
    edit::overwriteEdit(s, {{v1, a}, {v1, b}});
    REQUIRE(edit::setSpeed(s, a.id, 2.0, false, true, edit::unlimited));
    REQUIRE(s.clip(a.id)->duration == 50);
    REQUIRE(s.clip(b.id)->start == 50);
    REQUIRE(s.clip(a.id)->sourceAt(25) == 50);
}

TEST_CASE("document undo/redo and serialization")
{
    static int argc = 1;
    static char name[] = "tests";
    static char *argv[] = {name, nullptr};
    static QCoreApplication app(argc, argv);
    Document doc;
    REQUIRE(doc.project().sequences.size() == 1);
    doc.editSequence("add", [](Sequence &s) {
        return edit::overwriteEdit(s, {{{TrackType::Video, 0}, mk(0, 100)}});
    });
    REQUIRE(doc.project().activeSequence()->videoTracks[0].clips.size() == 1);
    doc.undo();
    REQUIRE(doc.project().activeSequence()->videoTracks[0].clips.empty());
    doc.redo();
    REQUIRE(doc.project().activeSequence()->videoTracks[0].clips.size() == 1);
    QByteArray data = Document::serialize(doc.project(), "/tmp");
    Project p;
    REQUIRE(Document::deserialize(data, "/tmp", p));
    REQUIRE(p.activeSequence()->videoTracks[0].clips[0].duration == 100);
}
