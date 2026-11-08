#include <QOpenGLFunctions>
#include <QOpenGLExtraFunctions>
#include "Monitor.h"
#include "Session.h"
#include "Icons.h"
#include "render/Renderer.h"
#include "media/MediaPool.h"
#include <QComboBox>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QPainter>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

namespace rc {

MonitorView::MonitorView(Playback::Mode mode, QWidget *parent) : QOpenGLWidget(parent), m_mode(mode)
{
    QSurfaceFormat f;
    f.setVersion(3, 3);
    f.setProfile(QSurfaceFormat::CoreProfile);
    setFormat(f);
    setMinimumSize(240, 135);
    setAcceptDrops(true);
    setMouseTracking(true);
    Session *s = Session::instance();
    if (mode == Playback::Mode::Program) {
        connect(s, &Session::playheadChanged, this, [this] { update(); });
        connect(&s->doc(), &Document::changed, this, [this] { update(); });
        connect(s, &Session::sequenceChanged, this, [this] { update(); });
        connect(s, &Session::selectionChanged, this, [this] { update(); });
    } else {
        connect(s, &Session::sourcePositionChanged, this, [this] { update(); });
        connect(s, &Session::sourceChanged, this, [this] { update(); });
    }
}

MonitorView::~MonitorView()
{
    makeCurrent();
    m_renderer.reset();
    doneCurrent();
}

void MonitorView::initializeGL()
{
    m_renderer = std::make_unique<Renderer>();
}

QSize MonitorView::contentSize() const
{
    Session *s = Session::instance();
    if (m_mode == Playback::Mode::Program) {
        if (const Sequence *seq = s->sequence())
            return {seq->width, seq->height};
    } else if (const MediaItem *m = s->project().mediaItem(s->sourceMedia())) {
        bool r = m->info.rotation % 180 != 0;
        return {int((r ? m->info.height : m->info.width) * m->info.pixelAspect), r ? m->info.width : m->info.height};
    }
    return {1920, 1080};
}

QRectF MonitorView::videoRect() const
{
    QSize c = contentSize();
    if (c.isEmpty())
        c = {16, 9};
    const double W = width(), H = height();
    double s = std::min(W / c.width(), H / c.height()) * 0.98;
    double w = c.width() * s, h = c.height() * s;
    return {(W - w) / 2, (H - h) / 2, w, h};
}

QRectF MonitorView::clipBounds(const Clip &c) const
{
    // Approximate bounding box of the clip after Motion (no rotation).
    Session *s = Session::instance();
    const Sequence *seq = s->sequence();
    if (!seq)
        return {};
    const Ticks local = seq->playhead - c.start;
    const EffectInstance *mo = c.effect("motion");
    if (!mo)
        return {};
    double nw = seq->width, nh = seq->height;
    if (c.source == ClipSource::Media)
        if (const MediaItem *m = s->project().mediaItem(c.mediaId)) {
            bool r = m->info.rotation % 180 != 0;
            nw = (r ? m->info.height : m->info.width) * m->info.pixelAspect;
            nh = r ? m->info.width : m->info.height;
        }
    double fit = 1;
    int mode = mo->value("fit", local).i();
    if (c.source == ClipSource::Media || c.source == ClipSource::Sequence)
        fit = mode == 1 ? std::max(seq->width / nw, seq->height / nh) : mode == 3 ? 1.0 : std::min(seq->width / nw, seq->height / nh);
    else
        fit = seq->width / nw;
    double sc = mo->f("scale", local) / 100.0 * fit;
    double scw = (mo->value("uniform", local).b() ? mo->f("scale", local) : mo->f("scaleWidth", local)) / 100.0 * fit;
    QPointF pos = mo->value("position", local).pt();
    QPointF anc = mo->value("anchor", local).pt();
    double w = nw * scw, h = nh * sc;
    double x = pos.x() * seq->width - anc.x() * w, y = pos.y() * seq->height - anc.y() * h;
    QRectF vr = videoRect();
    double k = vr.width() / seq->width;
    return {vr.x() + x * k, vr.y() + y * k, w * k, h * k};
}

void MonitorView::paintGL()
{
    Session *s = Session::instance();
    auto *f = context()->functions();
    const qreal dpr = devicePixelRatioF();
    f->glClearColor(0.07f, 0.07f, 0.08f, 1.f);
    f->glClear(GL_COLOR_BUFFER_BIT);
    if (m_renderer && m_renderer->ok())
        m_renderer->gl().bindState();
    if (!m_renderer || !m_renderer->ok()) {
        QPainter p(this);
        p.setPen(Qt::white);
        p.drawText(rect(), Qt::AlignCenter, tr("OpenGL 3.3 is not available on this system"));
        return;
    }
    QRectF vr = videoRect();
    if (m_zoom > 1.01f)
        vr = QRectF(0, 0, width(), height());
    const int outW = std::max(16, int(vr.width() * dpr * std::max(1.0f, m_zoom) / m_divider));
    const QSize cs = contentSize();
    const int outH = std::max(9, int(int64_t(outW) * cs.height() / std::max(1, cs.width())));
    SurfaceRef out;
    if (m_mode == Playback::Mode::Program) {
        if (const Sequence *seq = s->sequence()) {
            RenderOptions o;
            o.width = std::min(outW, seq->width);
            o.height = std::min(outH, seq->height);
            o.keepAlpha = m_checker;
            out = m_renderer->renderSequence(s->project(), *seq, seq->playhead, o);
        }
    } else if (const MediaItem *m = s->project().mediaItem(s->sourceMedia())) {
        if (m->info.hasVideo)
            out = m_renderer->renderMedia(s->project(), *m, s->sourcePosition(), std::min(outW, std::max(16, cs.width())),
                                          std::min(outH, std::max(9, cs.height())));
    }
    f->glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    if (out) {
        QRectF px(vr.x() * dpr, vr.y() * dpr, vr.width() * dpr, vr.height() * dpr);
        m_renderer->present(out, int(width() * dpr), int(height() * dpr), px, m_checker, m_zoom);
        if (m_scopes) {
            qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - m_lastScope > 90) {
                m_lastScope = now;
                emit frameForScopes(m_renderer->gl().readback(m_renderer->downscale(out, 360)));
            }
        }
    }
    f->glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    // Restore the default GL state QPainter expects (the renderer uses byte
    // aligned transfers, its own VAO and program).
    f->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    f->glPixelStorei(GL_PACK_ALIGNMENT, 4);
    f->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    f->glUseProgram(0);
    if (auto *ef = context()->extraFunctions()) ef->glBindVertexArray(0);
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_2D, 0);
    f->glDisable(GL_BLEND);
    f->glDisable(GL_SCISSOR_TEST);
    // Overlays.
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (m_mode == Playback::Mode::Source && s->sourceMedia().isEmpty()) {
        p.setPen(QColor(150, 150, 160));
        p.drawText(rect(), Qt::AlignCenter, tr("Double-click a clip in the Project panel\nto open it here"));
    }
    if (m_mode == Playback::Mode::Source)
        if (const MediaItem *m = s->project().mediaItem(s->sourceMedia()); m && !m->info.hasVideo) {
            p.setPen(QColor(120, 200, 140));
            p.drawText(rect(), Qt::AlignCenter, tr("Audio only\n%1").arg(m->name));
        }
    if (m_safe) {
        p.setPen(QPen(QColor(255, 255, 255, 120), 1, Qt::DashLine));
        auto inset = [&](double k) { return QRectF(vr.x() + vr.width() * k, vr.y() + vr.height() * k, vr.width() * (1 - 2 * k), vr.height() * (1 - 2 * k)); };
        p.drawRect(inset(0.05));
        p.drawRect(inset(0.1));
        p.drawLine(QPointF(vr.center().x() - 10, vr.center().y()), QPointF(vr.center().x() + 10, vr.center().y()));
        p.drawLine(QPointF(vr.center().x(), vr.center().y() - 10), QPointF(vr.center().x(), vr.center().y() + 10));
    }
    if (m_mode == Playback::Mode::Program && s->selection().size() >= 1) {
        if (Sequence *seq = s->sequence()) {
            const Clip *c = seq->clip(s->primaryClip());
            auto ref = seq->findClip(s->primaryClip());
            if (c && ref && ref->type == TrackType::Video && seq->playhead >= c->start && seq->playhead < c->end()) {
                QRectF b = clipBounds(*c);
                p.setPen(QPen(QColor(90, 160, 255), 1));
                p.setBrush(Qt::NoBrush);
                p.drawRect(b);
                p.setBrush(QColor(90, 160, 255));
                for (QPointF h : {b.topLeft(), b.topRight(), b.bottomLeft(), b.bottomRight()})
                    p.drawRect(QRectF(h.x() - 3, h.y() - 3, 6, 6));
            }
        }
    }
    if (!m_compare.isNull()) {
        // Split comparison: left half shows the reference frame.
        QRectF left(vr.x(), vr.y(), vr.width() / 2, vr.height());
        p.drawImage(left, m_compare, QRectF(0, 0, m_compare.width() / 2.0, m_compare.height()));
        p.setPen(QColor(255, 220, 0));
        p.drawLine(QPointF(vr.center().x(), vr.top()), QPointF(vr.center().x(), vr.bottom()));
    }
}

QImage MonitorView::grabCurrent()
{
    makeCurrent();
    Session *s = Session::instance();
    QImage img;
    if (m_renderer && m_renderer->ok()) {
        if (m_mode == Playback::Mode::Program) {
            if (const Sequence *seq = s->sequence()) {
                RenderOptions o;
                o.width = seq->width;
                o.height = seq->height;
                o.exporting = true;
                img = m_renderer->gl().readback(m_renderer->renderSequence(s->project(), *seq, seq->playhead, o));
            }
        } else if (const MediaItem *m = s->project().mediaItem(s->sourceMedia())) {
            QSize c = contentSize();
            img = m_renderer->gl().readback(m_renderer->renderMedia(s->project(), *m, s->sourcePosition(), c.width(), c.height(), false));
        }
    }
    doneCurrent();
    return img;
}

void MonitorView::mousePressEvent(QMouseEvent *e)
{
    Session *s = Session::instance();
    if (m_mode != Playback::Mode::Program || e->button() != Qt::LeftButton)
        return QOpenGLWidget::mousePressEvent(e);
    Sequence *seq = s->sequence();
    const Clip *c = seq ? seq->clip(s->primaryClip()) : nullptr;
    if (!c || !c->effect("motion"))
        return;
    QRectF b = clipBounds(*c);
    m_dragHandle = -2;
    QPointF corners[4] = {b.topLeft(), b.topRight(), b.bottomLeft(), b.bottomRight()};
    for (int i = 0; i < 4; ++i)
        if (QLineF(corners[i], e->position()).length() < 8)
            m_dragHandle = i;
    if (m_dragHandle == -2 && b.contains(e->position()))
        m_dragHandle = -1;
    if (m_dragHandle == -2)
        return;
    m_dragging = true;
    m_dragStart = e->position();
    const Ticks local = seq->playhead - c->start;
    m_startPos = c->effect("motion")->value("position", local);
    m_startScale = c->effect("motion")->value("scale", local);
}

void MonitorView::mouseMoveEvent(QMouseEvent *e)
{
    if (!m_dragging)
        return;
    Session *s = Session::instance();
    Sequence *seq = s->sequence();
    if (!seq)
        return;
    QRectF vr = videoRect();
    const QPointF d = e->position() - m_dragStart;
    const QString id = s->primaryClip();
    const Ticks local = seq->playhead - (seq->clip(id) ? seq->clip(id)->start : 0);
    s->doc().editMerged(tr("Move in Program Monitor"), "monitor-drag-" + id, [&](Project &p) {
        Clip *c = p.activeSequence()->clip(id);
        if (!c) return false;
        EffectInstance *mo = c->effect("motion");
        if (!mo) return false;
        if (m_dragHandle == -1) {
            Value v = m_startPos;
            v.v[0] += d.x() / vr.width();
            v.v[1] += d.y() / vr.height();
            mo->param("position").setValue(v, local);
        } else {
            double k = 1.0 + (std::abs(d.x()) > std::abs(d.y()) ? d.x() : d.y()) / std::max(50.0, vr.width() / 2) * ((m_dragHandle == 0 || m_dragHandle == 2) ? -1 : 1);
            mo->param("scale").setValue(Value(std::max(1.0, m_startScale.f() * k)), local);
        }
        return true;
    });
}

void MonitorView::mouseReleaseEvent(QMouseEvent *)
{
    m_dragging = false;
    Session::instance()->doc().endMerge();
}

void MonitorView::wheelEvent(QWheelEvent *e)
{
    Session *s = Session::instance();
    int steps = e->angleDelta().y() > 0 ? -1 : 1;
    if (m_mode == Playback::Mode::Program) {
        if (Sequence *seq = s->sequence())
            s->setPlayhead(seq->playhead + steps * seq->fps.frameTicks());
    } else if (const MediaItem *m = s->project().mediaItem(s->sourceMedia())) {
        s->setSourcePosition(s->sourcePosition() + steps * m->fps().frameTicks());
    }
}

void MonitorView::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void MonitorView::dropEvent(QDropEvent *e)
{
    QStringList paths;
    for (const auto &u : e->mimeData()->urls())
        if (u.isLocalFile())
            paths << u.toLocalFile();
    emit droppedFiles(paths);
}

// ---------------------------------------------------------------------------

MonitorPanel::MonitorPanel(Playback::Mode mode, QWidget *parent) : QWidget(parent), m_mode(mode)
{
    Session *s = Session::instance();
    m_playback = new Playback(mode, this);
    m_view = new MonitorView(mode, this);
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(2, 2, 2, 2);
    lay->setSpacing(2);
    m_title = new QLabel(this);
    m_title->setObjectName("monitorTitle");
    lay->addWidget(m_title);
    lay->addWidget(m_view, 1);
    m_scrub = new QSlider(Qt::Horizontal, this);
    m_scrub->setRange(0, 10000);
    lay->addWidget(m_scrub);
    auto *bar = new QHBoxLayout;
    m_tc = new QLabel("00:00:00:00", this);
    m_tc->setObjectName("timecode");
    bar->addWidget(m_tc);
    m_zoom = new QComboBox(this);
    m_zoom->addItems({tr("Fit"), "100%", "200%", "400%"});
    bar->addWidget(m_zoom);
    m_res = new QComboBox(this);
    m_res->addItems({tr("Full"), "1/2", "1/4", "1/8"});
    m_res->setToolTip(tr("Playback resolution"));
    bar->addWidget(m_res);
    bar->addStretch();
    auto button = [&](const QString &icon, const QString &tip, auto fn) {
        auto *b = new QToolButton(this);
        b->setIcon(Icons::get(icon));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        connect(b, &QToolButton::clicked, this, fn);
        bar->addWidget(b);
        return b;
    };
    button("mark-in", tr("Mark In (I)"), [this] { markIn(); });
    button("mark-out", tr("Mark Out (O)"), [this] { markOut(); });
    button("go-in", tr("Go to In (Shift+I)"), [this] { goToIn(); });
    button("step-back", tr("Step Back (Left)"), [this] { stepFrames(-1); });
    m_playBtn = button("play", tr("Play/Stop (Space)"), [this] { m_playback->toggle(); });
    button("step-forward", tr("Step Forward (Right)"), [this] { stepFrames(1); });
    button("go-out", tr("Go to Out (Shift+O)"), [this] { goToOut(); });
    auto *loop = button("loop", tr("Loop"), [this] {});
    loop->setCheckable(true);
    connect(loop, &QToolButton::toggled, m_playback, &Playback::setLoop);
    if (mode == Playback::Mode::Source) {
        button("insert", tr("Insert (,)"), [this] { emit insertRequested(); });
        button("overwrite", tr("Overwrite (.)"), [this] { emit overwriteRequested(); });
    } else {
        button("camera", tr("Export Frame (Shift+E)"), [this] { emit exportFrameRequested(); });
        auto *safe = button("safe", tr("Safe Margins"), [this] {});
        safe->setCheckable(true);
        connect(safe, &QToolButton::toggled, m_view, &MonitorView::setSafeMargins);
    }
    bar->addStretch();
    m_dur = new QLabel(this);
    m_dur->setObjectName("timecodeDim");
    bar->addWidget(m_dur);
    lay->addLayout(bar);

    connect(m_zoom, &QComboBox::currentIndexChanged, this, [this](int i) { m_view->setZoom(i == 0 ? 1.f : float(1 << (i - 1))); });
    connect(m_res, &QComboBox::currentIndexChanged, this, [this](int i) { m_view->setResolutionDivider(1 << i); });
    connect(m_playback, &Playback::stateChanged, this, [this](bool p) { m_playBtn->setIcon(Icons::get(p ? "pause" : "play")); });
    connect(m_scrub, &QSlider::valueChanged, this, [this, s](int v) {
        if (m_updatingScrub)
            return;
        if (m_mode == Playback::Mode::Program) {
            if (Sequence *seq = s->sequence())
                s->setPlayhead(Ticks(double(v) / 10000.0 * std::max<Ticks>(seq->duration(), kTicksPerSecond)));
        } else if (const MediaItem *m = s->project().mediaItem(s->sourceMedia())) {
            s->setSourcePosition(floorToFrame(Ticks(double(v) / 10000.0 * m->info.duration), m->fps()));
        }
    });
    if (mode == Playback::Mode::Program) {
        connect(s, &Session::playheadChanged, this, &MonitorPanel::refresh);
        connect(&s->doc(), &Document::changed, this, &MonitorPanel::refresh);
        connect(s, &Session::sequenceChanged, this, &MonitorPanel::refresh);
    } else {
        connect(s, &Session::sourcePositionChanged, this, &MonitorPanel::refresh);
        connect(s, &Session::sourceChanged, this, &MonitorPanel::refresh);
    }
    refresh();
}

void MonitorPanel::refresh()
{
    Session *s = Session::instance();
    Ticks pos = 0, dur = 0, in = kInvalidTicks, out = kInvalidTicks;
    FrameRate fps{25, 1};
    QString title;
    if (m_mode == Playback::Mode::Program) {
        if (const Sequence *seq = s->sequence()) {
            pos = seq->playhead;
            dur = seq->duration();
            fps = seq->fps;
            in = seq->inPoint;
            out = seq->outPoint;
            title = tr("Program: %1").arg(seq->name);
        }
    } else if (const MediaItem *m = s->project().mediaItem(s->sourceMedia())) {
        pos = s->sourcePosition();
        dur = m->info.duration;
        fps = m->fps();
        in = m->inPoint;
        out = m->outPoint;
        title = tr("Source: %1").arg(m->name);
    } else {
        title = tr("Source: (no clip)");
    }
    m_title->setText(title);
    m_tc->setText(formatTimecode(pos, fps));
    Ticks shown = (in != kInvalidTicks || out != kInvalidTicks) ? (out != kInvalidTicks ? out : dur) - (in != kInvalidTicks ? in : 0) : dur;
    m_dur->setText(formatTimecode(std::max<Ticks>(0, shown), fps));
    m_updatingScrub = true;
    m_scrub->setValue(dur > 0 ? int(double(pos) / dur * 10000) : 0);
    m_updatingScrub = false;
}

void MonitorPanel::stepFrames(int n)
{
    Session *s = Session::instance();
    m_playback->stop();
    if (m_mode == Playback::Mode::Program) {
        if (Sequence *seq = s->sequence())
            s->setPlayhead(seq->playhead + n * seq->fps.frameTicks());
    } else if (const MediaItem *m = s->project().mediaItem(s->sourceMedia())) {
        s->setSourcePosition(std::min(m->info.duration, s->sourcePosition() + n * m->fps().frameTicks()));
    }
}

void MonitorPanel::markIn()
{
    Session *s = Session::instance();
    if (m_mode == Playback::Mode::Program) {
        s->doc().editSequence(tr("Mark In"), [&](Sequence &seq) { seq.inPoint = seq.playhead; return true; });
    } else {
        const QString id = s->sourceMedia();
        Ticks pos = s->sourcePosition();
        s->doc().edit(tr("Mark In"), [&](Project &p) {
            MediaItem *m = p.mediaItem(id);
            if (!m) return false;
            m->inPoint = pos;
            if (m->outPoint != kInvalidTicks && m->outPoint <= pos) m->outPoint = kInvalidTicks;
            return true;
        });
    }
    refresh();
}

void MonitorPanel::markOut()
{
    Session *s = Session::instance();
    if (m_mode == Playback::Mode::Program) {
        s->doc().editSequence(tr("Mark Out"), [&](Sequence &seq) { seq.outPoint = seq.playhead + seq.fps.frameTicks(); return true; });
    } else {
        const QString id = s->sourceMedia();
        Ticks pos = s->sourcePosition();
        s->doc().edit(tr("Mark Out"), [&](Project &p) {
            MediaItem *m = p.mediaItem(id);
            if (!m) return false;
            m->outPoint = pos + m->fps().frameTicks();
            if (m->inPoint != kInvalidTicks && m->inPoint >= m->outPoint) m->inPoint = kInvalidTicks;
            return true;
        });
    }
    refresh();
}

void MonitorPanel::clearInOut()
{
    Session *s = Session::instance();
    if (m_mode == Playback::Mode::Program) {
        s->doc().editSequence(tr("Clear In and Out"), [&](Sequence &seq) { seq.inPoint = seq.outPoint = kInvalidTicks; return true; });
    } else {
        const QString id = s->sourceMedia();
        s->doc().edit(tr("Clear In and Out"), [&](Project &p) {
            MediaItem *m = p.mediaItem(id);
            if (!m) return false;
            m->inPoint = m->outPoint = kInvalidTicks;
            return true;
        });
    }
    refresh();
}

void MonitorPanel::goToIn()
{
    Session *s = Session::instance();
    if (m_mode == Playback::Mode::Program) {
        if (Sequence *seq = s->sequence(); seq && seq->inPoint != kInvalidTicks)
            s->setPlayhead(seq->inPoint);
    } else if (const MediaItem *m = s->project().mediaItem(s->sourceMedia()); m && m->inPoint != kInvalidTicks) {
        s->setSourcePosition(m->inPoint);
    }
}

void MonitorPanel::goToOut()
{
    Session *s = Session::instance();
    if (m_mode == Playback::Mode::Program) {
        if (Sequence *seq = s->sequence(); seq && seq->outPoint != kInvalidTicks)
            s->setPlayhead(seq->outPoint - seq->fps.frameTicks());
    } else if (const MediaItem *m = s->project().mediaItem(s->sourceMedia()); m && m->outPoint != kInvalidTicks) {
        s->setSourcePosition(m->outPoint - m->fps().frameTicks());
    }
}

} // namespace rc
