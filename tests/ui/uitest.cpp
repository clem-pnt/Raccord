// Drives the real main window with simulated input and checks the model.
// Usage: raccord-uitest <project.raccord> <screenshot dir>
#include "ui/MainWindow.h"
#include "ui/Icons.h"
#include "ui/Monitor.h"
#include "ui/Session.h"
#include "ui/Timeline.h"
#include "media/FFmpeg.h"
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QPushButton>
#include <QSurfaceFormat>
#include <QTest>
#include <QTimer>
#include <cstdio>

using namespace rc;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } else std::fprintf(stderr, "ok   %s\n", #cond); } while (0)

static QAction *findAction(QWidget *w, const QString &text)
{
    for (QAction *a : w->findChildren<QAction *>())
        if (a->text().remove('&') == text) return a;
    for (QAction *a : w->actions())
        if (a->text().remove('&') == text) return a;
    return nullptr;
}

static int clipCount(const Sequence *s, TrackType t, int track)
{
    return s ? int(s->tracks(t)[track].clips.size()) : -1;
}

int main(int argc, char **argv)
{
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(fmt);
    QApplication app(argc, argv);
    QApplication::setApplicationName("Raccord-uitest");
    QApplication::setOrganizationName("Raccord-uitest");
    if (argc < 3) {
        std::fprintf(stderr, "usage: raccord-uitest <project> <outdir>\n");
        return 2;
    }
    const QString outDir = argv[2];
    QDir().mkpath(outDir);
    ff::initLogging();
    applyTheme(app);
    Session session;
    MainWindow w;
    w.resize(1920, 1050);
    w.show();
    QTest::qWait(500);
    CHECK(w.openProject(QString::fromLocal8Bit(argv[1])));
    QTest::qWait(1500);
    auto shot = [&](const QString &name) {
        QTest::qWait(1200);
        w.grab().save(outDir + "/" + name + ".png");
    };
    shot("01-editing");
    for (MonitorView *mv : w.findChildren<MonitorView *>())
        QObject::connect(mv, &MonitorView::frameForScopes, [outDir](const QImage &img) { img.save(outDir + "/scope-frame.png"); });

    TimelineView *tl = w.findChild<TimelineView *>();
    CHECK(tl != nullptr);
    tl->zoomToFit();
    QTest::qWait(100);
    const Sequence *seq = session.sequence();
    CHECK(seq && seq->videoTracks.size() >= 3);
    const double pps = tl->pixelsPerSecond();
    auto xAt = [&](double sec) { return int(190 + sec * pps); };
    const int yV1 = 30 + 52 * 2 + 30;   // V3, V2 rows then middle of V1
    const int yA1 = 30 + 52 * 3 + 8 + 30;
    const int v1Before = clipCount(seq, TrackType::Video, 0);

    // 1. Click selects a clip and its linked audio.
    QTest::mouseClick(tl, Qt::LeftButton, {}, QPoint(xAt(1.5), yV1));
    QTest::qWait(50);
    CHECK(session.selection().size() == 2);

    // 2. Razor tool (C) cuts V1 + A1 at 2.5 s.
    session.setTool(Tool::Razor);
    QTest::mouseClick(tl, Qt::LeftButton, {}, QPoint(xAt(2.5), yV1));
    QTest::qWait(50);
    seq = session.sequence();
    CHECK(clipCount(seq, TrackType::Video, 0) == v1Before + 1);
    session.setTool(Tool::Selection);

    // 3. Delete the second half, then undo.
    QTest::mouseClick(tl, Qt::LeftButton, {}, QPoint(xAt(3.0), yV1));
    QTest::qWait(50);
    tl->setFocus();
    QAction *clear = findAction(&w, "Clear");
    CHECK(clear != nullptr);
    if (clear) clear->trigger();
    seq = session.sequence();
    CHECK(clipCount(seq, TrackType::Video, 0) == v1Before);
    session.doc().undo();
    seq = session.sequence();
    CHECK(clipCount(seq, TrackType::Video, 0) == v1Before + 1);

    // 4. Drag a clip to V2 (move with track change).
    const Ticks oldStart = seq->videoTracks[0].clips[0].start;
    QTest::mousePress(tl, Qt::LeftButton, {}, QPoint(xAt(1.0), yV1));
    for (int i = 1; i <= 10; ++i) {
        QMouseEvent mv(QEvent::MouseMove, QPointF(xAt(1.0) + i * 4, yV1 - i * 5), QPointF(), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tl, &mv);
        QTest::qWait(10);
    }
    QTest::mouseRelease(tl, Qt::LeftButton, {}, QPoint(xAt(1.0) + 40, yV1 - 50));
    QTest::qWait(50);
    seq = session.sequence();
    CHECK(clipCount(seq, TrackType::Video, 1) >= 2);
    session.doc().undo();
    seq = session.sequence();
    CHECK(seq->videoTracks[0].clips[0].start == oldStart);

    // 5. Ruler scrub moves the playhead.
    QTest::mouseClick(tl, Qt::LeftButton, {}, QPoint(xAt(4.0), 15));
    CHECK(std::abs(ticksToSeconds(session.playhead()) - 4.0) < 0.2);
    shot("02-after-edits");

    // 6. Keyboard: Right arrow steps one frame; Mark In/Out.
    const Ticks ph = session.playhead();
    if (QAction *a = findAction(&w, "Step Forward")) a->trigger();
    CHECK(session.playhead() == ph + seq->fps.frameTicks());
    if (QAction *a = findAction(&w, "Mark In")) a->trigger();
    CHECK(session.sequence()->inPoint == session.playhead());

    // 7. Select a clip and look at Effect Controls, then switch workspaces.
    QTest::mouseClick(tl, Qt::LeftButton, {}, QPoint(xAt(1.0), yV1));
    session.setPlayhead(secondsToTicks(1.0));
    for (QDockWidget *d : w.findChildren<QDockWidget *>())
        if (d->objectName() == "controls") { d->show(); d->raise(); }
    shot("03-effect-controls");
    if (QAction *a = findAction(&w, "Color")) a->trigger();
    QTest::qWait(300);
    for (QPushButton *b : w.findChildren<QPushButton *>())
        if (b->text() == "Apply Lumetri Color" && b->isVisible()) { b->click(); break; }
    QTest::qWait(300);
    {
        const Clip *c = session.sequence()->clip(session.primaryClip());
        CHECK(c && c->effect("lumetri"));
        // Warm, contrasty grade so the screenshot shows the effect.
        session.doc().editSequence("grade", [&](Sequence &q) {
            Clip *cc = q.clip(session.primaryClip());
            if (!cc) return false;
            EffectInstance *l = cc->effect("lumetri");
            l->param("temperature").setStaticValue(Value(40.0));
            l->param("contrast").setStaticValue(Value(30.0));
            l->param("saturation").setStaticValue(Value(130.0));
            return true;
        });
    }
    shot("04-color");
    for (MonitorPanel *m : w.findChildren<MonitorPanel *>())
        if (m->mode() == Playback::Mode::Program) m->view()->grabCurrent().save(outDir + "/grade-check.png");
    if (QAction *a = findAction(&w, "Audio")) a->trigger();
    shot("05-audio");
    if (QAction *a = findAction(&w, "Effects")) a->trigger();
    shot("06-effects");
    if (QAction *a = findAction(&w, "Editing")) a->trigger();

    // 8. Playback advances the playhead (audio output may be absent on servers).
    session.setPlayhead(0);
    MonitorPanel *program = nullptr;
    for (MonitorPanel *m : w.findChildren<MonitorPanel *>())
        if (m->mode() == Playback::Mode::Program) program = m;
    CHECK(program != nullptr);
    if (program) {
        program->playback()->play(1.0);
        QTest::qWait(1500);
        program->playback()->stop();
        std::fprintf(stderr, "     playhead after 1.5 s of playback: %.2f s\n", ticksToSeconds(session.playhead()));
        CHECK(session.playhead() > secondsToTicks(0.8));
    }
    shot("07-final");
    std::fprintf(stderr, "%d failure(s)\n", failures);
    session.doc().touch(false);
    QTimer::singleShot(0, &app, &QApplication::quit);
    // Do not prompt to save.
    return failures == 0 ? 0 : 1;
}
