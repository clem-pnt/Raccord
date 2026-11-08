#include "Timeline.h"
#include "Icons.h"
#include "Widgets.h"
#include "MediaCache.h"
#include "media/MediaProbe.h"
#include "audio/AudioEffects.h"
#include <QApplication>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

namespace rc {

static constexpr int kTrackH = 52;
static constexpr int kCaptionH = 30;
static constexpr int kSplit = 8;
static constexpr int kEdge = 6;   // trim handle width in px

TimelineView::TimelineView(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setAcceptDrops(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumHeight(160);
    m_hbar = new QScrollBar(Qt::Horizontal, this);
    m_vbar = new QScrollBar(Qt::Vertical, this);
    connect(m_hbar, &QScrollBar::valueChanged, this, [this](int v) {
        m_scroll = Ticks(double(v) / 100.0 * kTicksPerSecond);
        update();
    });
    connect(m_vbar, &QScrollBar::valueChanged, this, [this](int v) {
        m_scrollY = v;
        update();
    });
    Session *s = Session::instance();
    connect(&s->doc(), &Document::changed, this, [this] { updateScrollbars(); update(); });
    connect(s, &Session::playheadChanged, this, [this] { ensurePlayheadVisible(); update(); });
    connect(s, &Session::selectionChanged, this, [this] { update(); });
    connect(s, &Session::sequenceChanged, this, [this] { m_scroll = 0; zoomToFit(); });
    connect(s, &Session::toolChanged, this, [this] { update(); });
    connect(MediaCache::instance(), &MediaCache::updated, this, [this] { update(); });
}

const Sequence *TimelineView::viewSequence() const
{
    if (m_preview)
        return &*m_preview;
    return Session::instance()->sequence();
}

std::vector<TimelineView::Row> TimelineView::rows() const
{
    std::vector<Row> out;
    const Sequence *seq = viewSequence();
    if (!seq)
        return out;
    int y = trackAreaTop() - m_scrollY;
    for (int i = 0; i < (int)seq->captionTracks.size(); ++i) {
        out.push_back({Row::Caption, i, y, kCaptionH});
        y += kCaptionH;
    }
    for (int i = (int)seq->videoTracks.size() - 1; i >= 0; --i) {
        int h = seq->videoTracks[i].height > 0 ? seq->videoTracks[i].height : kTrackH;
        out.push_back({Row::Video, i, y, h});
        y += h;
    }
    y += kSplit;
    for (int i = 0; i < (int)seq->audioTracks.size(); ++i) {
        int h = seq->audioTracks[i].height > 0 ? seq->audioTracks[i].height : kTrackH;
        out.push_back({Row::Audio, i, y, h});
        y += h;
    }
    return out;
}

const TimelineView::Row *TimelineView::rowAt(int y) const
{
    static std::vector<Row> cache;
    cache = rows();
    for (const auto &r : cache)
        if (y >= r.y && y < r.y + r.h)
            return &r;
    return nullptr;
}

Ticks TimelineView::xToTicks(double x) const
{
    return m_scroll + Ticks((x - headerWidth()) / m_pps * double(kTicksPerSecond));
}

double TimelineView::ticksToX(Ticks t) const
{
    return headerWidth() + double(t - m_scroll) / double(kTicksPerSecond) * m_pps;
}

QRect TimelineView::clipRect(const Row &r, const Clip &c) const
{
    int x0 = int(std::floor(ticksToX(c.start)));
    int x1 = int(std::floor(ticksToX(c.end())));
    return QRect(x0, r.y + 1, std::max(1, x1 - x0), r.h - 2);
}

Ticks TimelineView::snap(Ticks t, const std::set<QString> &exclude, bool *snapped) const
{
    if (snapped) *snapped = false;
    Session *s = Session::instance();
    const Sequence *seq = m_origin ? &*m_origin : s->sequence();
    if (!seq)
        return t;
    if (!s->snapping() != bool(QApplication::keyboardModifiers() & Qt::ShiftModifier && m_drag != Drag::Move))
        ; // shift is used for other things during moves
    if (!s->snapping())
        return t;
    const Ticks tol = Ticks(8.0 / m_pps * kTicksPerSecond);
    Ticks best = t, bestD = tol + 1;
    for (Ticks p : edit::snapPoints(*seq, exclude)) {
        Ticks d = std::llabs(p - t);
        if (d < bestD) { bestD = d; best = p; }
    }
    if (bestD <= tol) {
        if (snapped) *snapped = true;
        return best;
    }
    return t;
}

TimelineView::Hit TimelineView::hitTest(QPoint p) const
{
    Hit h;
    h.time = xToTicks(p.x());
    if (p.y() < rulerHeight()) {
        h.part = p.x() < headerWidth() ? Hit::None : Hit::Ruler;
        return h;
    }
    const Sequence *seq = viewSequence();
    if (!seq)
        return h;
    auto rs = rows();
    for (int ri = 0; ri < (int)rs.size(); ++ri) {
        const Row &r = rs[ri];
        if (p.y() < r.y || p.y() >= r.y + r.h)
            continue;
        h.row = ri;
        if (p.x() < headerWidth()) {
            h.part = Hit::Header;
            return h;
        }
        h.part = Hit::Empty;
        if (r.kind == Row::Caption) {
            const auto &ct = seq->captionTracks[r.index];
            for (int i = 0; i < (int)ct.captions.size(); ++i) {
                const auto &c = ct.captions[i];
                double x0 = ticksToX(c.start), x1 = ticksToX(c.end);
                if (p.x() >= x0 - 3 && p.x() <= x1 + 3) {
                    h.captionIndex = i;
                    h.part = p.x() < x0 + kEdge ? Hit::CaptionIn : p.x() > x1 - kEdge ? Hit::CaptionOut : Hit::Caption;
                    return h;
                }
            }
            return h;
        }
        const Track &t = seq->tracks(r.kind == Row::Video ? TrackType::Video : TrackType::Audio)[r.index];
        // Transitions first (they sit over the cut).
        for (const auto &tr : t.transitions) {
            TimeRange rg = edit::transitionRange(t, tr);
            double x0 = ticksToX(rg.start), x1 = ticksToX(rg.end);
            if (p.x() >= x0 && p.x() <= x1 && p.y() < r.y + r.h / 2 + 8) {
                h.transitionId = tr.id;
                h.part = (p.x() - x0 < 5 || x1 - p.x() < 5) ? Hit::TransitionEdge : Hit::Transition;
                if (h.part == Hit::TransitionEdge)
                    h.clipId = p.x() - x0 < 5 ? QStringLiteral("<") : QStringLiteral(">");
                return h;
            }
        }
        for (const auto &c : t.clips) {
            QRect cr = clipRect(r, c);
            if (p.x() < cr.left() - 1 || p.x() > cr.right() + 1)
                continue;
            h.clipId = c.id;
            const int edge = std::min(kEdge, cr.width() / 3);
            if (p.x() <= cr.left() + edge) h.part = Hit::ClipIn;
            else if (p.x() >= cr.right() - edge) h.part = Hit::ClipOut;
            else h.part = Hit::ClipBody;
            if (r.kind == Row::Audio && h.part == Hit::ClipBody) {
                // Volume rubber band.
                const EffectInstance *v = c.effect("volume");
                double db = v ? v->f("level", h.time - c.start) : 0;
                double fy = 1.0 - std::clamp((db + 60.0) / 75.0, 0.0, 1.0);
                int ly = cr.top() + 14 + int(fy * (cr.height() - 16));
                if (std::abs(p.y() - ly) <= 3)
                    h.part = Hit::VolumeLine;
            }
            return h;
        }
        return h;
    }
    return h;
}

// ---------------------------------------------------------------------------
// Painting

void TimelineView::paintRuler(QPainter &p)
{
    const Sequence *seq = viewSequence();
    QRect r(headerWidth(), 0, width() - headerWidth(), rulerHeight());
    p.fillRect(r, QColor(30, 30, 34));
    if (!seq)
        return;
    const FrameRate fps = seq->fps;
    // Choose a tick spacing of at least 80 px.
    const double frameW = m_pps / fps.fps();
    static const double steps[] = {1.0 / 60, 1.0 / 30, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600};
    double step = 3600;
    for (double st : steps) {
        double s2 = st < 1 ? std::max(st, 1.0 / fps.fps()) : st;
        if (s2 * m_pps >= 90) { step = s2; break; }
    }
    const double t0 = ticksToSeconds(m_scroll);
    const double t1 = t0 + (width() - headerWidth()) / m_pps;
    p.setPen(QColor(130, 130, 140));
    QFont f = p.font();
    f.setPixelSize(10);
    p.setFont(f);
    for (double t = std::floor(t0 / step) * step; t <= t1; t += step) {
        double x = ticksToX(secondsToTicks(t));
        p.drawLine(QPointF(x, rulerHeight() - 10), QPointF(x, rulerHeight()));
        p.drawText(QPointF(x + 3, 12), formatTimecode(secondsToTicks(t) + seq->startTimecode, fps));
        // minor ticks
        for (int k = 1; k < 5; ++k) {
            double mx = x + k * step * m_pps / 5;
            p.drawLine(QPointF(mx, rulerHeight() - 4), QPointF(mx, rulerHeight()));
        }
    }
    if (frameW > 6) {
        p.setPen(QColor(80, 80, 88));
        for (int64_t fr = ticksToFrame(m_scroll, fps); ; ++fr) {
            double x = ticksToX(frameToTicks(fr, fps));
            if (x > width()) break;
            p.drawLine(QPointF(x, rulerHeight() - 2), QPointF(x, rulerHeight()));
        }
    }
    // In/out
    if (seq->inPoint != kInvalidTicks || seq->outPoint != kInvalidTicks) {
        double x0 = ticksToX(seq->inPoint != kInvalidTicks ? seq->inPoint : 0);
        double x1 = ticksToX(seq->outPoint != kInvalidTicks ? seq->outPoint : seq->duration());
        p.fillRect(QRectF(x0, rulerHeight() - 6, x1 - x0, 6), QColor(74, 144, 255, 150));
    }
    // Markers
    for (const auto &m : seq->markers) {
        double x = ticksToX(m.time);
        QColor c = labelColors()[std::clamp(m.color, 0, (int)labelColors().size() - 1)];
        QPolygonF poly({QPointF(x - 5, 14), QPointF(x + 5, 14), QPointF(x + 5, 22), QPointF(x, 27), QPointF(x - 5, 22)});
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPolygon(poly);
        if (m.duration > 0)
            p.fillRect(QRectF(x, 22, ticksToX(m.time + m.duration) - x, 4), c);
    }
}

static void drawWave(QPainter &p, const QRect &r, const Peaks &pk, const Clip &c, double pps, Ticks scroll, int headerX)
{
    if (pk.data.empty())
        return;
    p.setPen(QColor(120, 220, 160, 200));
    const int mid = r.center().y();
    const int half = r.height() / 2 - 2;
    const EffectInstance *v = c.effect("volume");
    for (int x = std::max(r.left(), headerX); x < r.right(); ++x) {
        Ticks t = scroll + Ticks(double(x - headerX) / pps * kTicksPerSecond);
        Ticks src = c.sourceAt(t);
        double sec = ticksToSeconds(src);
        Ticks t2 = scroll + Ticks(double(x + 1 - headerX) / pps * kTicksPerSecond);
        double sec2 = ticksToSeconds(c.sourceAt(t2));
        int i0 = int(std::min(sec, sec2) * pk.perSecond), i1 = int(std::max(sec, sec2) * pk.perSecond);
        float m = 0;
        for (int i = std::max(0, i0); i <= i1 && i < (int)pk.data.size(); ++i)
            m = std::max(m, pk.data[i]);
        if (v)
            m *= dbToGain(v->f("level", t - c.start));
        int h = int(std::min(1.0f, m) * half);
        p.drawLine(x, mid - h, x, mid + h);
    }
}

void TimelineView::paintClip(QPainter &p, const Row &row, const Track &track, const Clip &c, bool selected)
{
    Session *s = Session::instance();
    QRect r = clipRect(row, c);
    if (r.right() < headerWidth() || r.left() > width())
        return;
    QColor base = labelColors()[std::clamp(c.label, 0, (int)labelColors().size() - 1)];
    if (row.kind == Row::Audio && c.label == 0)
        base = QColor(0x3d, 0x9a, 0x6a);
    if (c.source == ClipSource::Title) base = QColor(0xb0, 0x6a, 0xd8);
    if (c.source == ClipSource::Adjustment) base = QColor(0x8a, 0x8a, 0x9a);
    if (c.source == ClipSource::Sequence) base = QColor(0x3a, 0x8a, 0x5a);
    QColor fill = base.darker(c.enabled ? 170 : 300);
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawRoundedRect(r, 2, 2);
    p.save();
    p.setClipRect(r.intersected(QRect(headerWidth(), 0, width(), height())));
    const MediaItem *m = c.source == ClipSource::Media ? s->project().mediaItem(c.mediaId) : nullptr;
    if (row.kind == Row::Video && m && m->info.hasVideo && r.width() > 40 && r.height() > 30) {
        const int th = r.height() - 16;
        const int tw = std::max(8, int(th * 16.0 / 9.0));
        QImage img = MediaCache::instance()->thumbnail(m->proxyPath.isEmpty() ? m->path : m->proxyPath, c.in, 96);
        if (!img.isNull() && img.width() > 1) {
            p.drawImage(QRect(r.left() + 1, r.top() + 15, tw, th), img);
            if (r.width() > tw * 3) {
                QImage img2 = MediaCache::instance()->thumbnail(m->proxyPath.isEmpty() ? m->path : m->proxyPath, c.sourceOut() - kTicksPerSecond, 96);
                if (!img2.isNull() && img2.width() > 1)
                    p.drawImage(QRect(r.right() - tw, r.top() + 15, tw, th), img2);
            }
        }
    }
    if (row.kind == Row::Audio && m && m->info.hasAudio) {
        if (auto pk = MediaCache::instance()->peaks(m->path, c.audioStream))
            drawWave(p, r.adjusted(0, 14, 0, 0), *pk, c, m_pps, m_scroll, headerWidth());
    }
    // Header strip with the name.
    p.fillRect(QRect(r.left(), r.top(), r.width(), 14), base.darker(c.enabled ? 115 : 250));
    p.setPen(c.enabled ? QColor(240, 240, 245) : QColor(140, 140, 145));
    QFont f = p.font();
    f.setPixelSize(11);
    p.setFont(f);
    QString name = c.name.isEmpty() ? clipSourceName(c.source) : c.name;
    if (c.speed != 1.0 || c.reverse)
        name += QString(" [%1%2%]").arg(c.reverse ? "-" : "").arg(c.speed * 100, 0, 'f', 0);
    bool hasFx = false;
    for (const auto &e : c.effects)
        if (const EffectDef *d = EffectRegistry::instance().find(e.type); d && !d->intrinsic) hasFx = true;
    if (hasFx)
        name = "fx  " + name;
    if (m && m->path.isEmpty() == false && !m->info.valid)
        name += tr(" (offline)");
    p.drawText(r.adjusted(4, 0, -4, 0).topLeft() + QPoint(0, 11), name);
    // Volume rubber band on audio clips.
    if (row.kind == Row::Audio) {
        const EffectInstance *v = c.effect("volume");
        if (v) {
            p.setPen(QPen(QColor(255, 220, 90), 1));
            auto yOf = [&](double db) { return r.top() + 14 + (1.0 - std::clamp((db + 60.0) / 75.0, 0.0, 1.0)) * (r.height() - 16); };
            const auto it = v->params.find("level");
            if (it != v->params.end() && it->second.animated()) {
                QPainterPath path;
                bool first = true;
                for (int x = std::max(r.left(), headerWidth()); x <= r.right(); x += 2) {
                    Ticks t = xToTicks(x) - c.start;
                    double y = yOf(it->second.valueAt(t).f());
                    if (first) { path.moveTo(x, y); first = false; } else path.lineTo(x, y);
                }
                p.setBrush(Qt::NoBrush);
                p.drawPath(path);
                p.setBrush(QColor(255, 220, 90));
                for (const auto &k : it->second.keys())
                    p.drawEllipse(QPointF(ticksToX(c.start + k.time), yOf(k.value.f())), 3, 3);
            } else {
                double y = yOf(v->f("level", 0));
                p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
            }
        }
    }
    // Opacity keyframes on video clips.
    if (row.kind == Row::Video) {
        if (const EffectInstance *o = c.effect("opacity")) {
            auto it = o->params.find("opacity");
            if (it != o->params.end() && it->second.animated()) {
                p.setPen(QPen(QColor(255, 220, 90), 1));
                QPainterPath path;
                bool first = true;
                auto yOf = [&](double v) { return r.top() + 14 + (1.0 - v / 100.0) * (r.height() - 16); };
                for (int x = std::max(r.left(), headerWidth()); x <= r.right(); x += 2) {
                    double y = yOf(it->second.valueAt(xToTicks(x) - c.start).f());
                    if (first) { path.moveTo(x, y); first = false; } else path.lineTo(x, y);
                }
                p.setBrush(Qt::NoBrush);
                p.drawPath(path);
            }
        }
    }
    p.restore();
    if (selected) {
        p.setPen(QPen(QColor(255, 255, 255), 2));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r.adjusted(1, 1, -1, -1), 2, 2);
    }
    (void)track;
}

void TimelineView::paintHeaders(QPainter &p, const std::vector<Row> &rs)
{
    const Sequence *seq = viewSequence();
    p.fillRect(QRect(0, rulerHeight(), headerWidth(), height()), QColor(36, 36, 41));
    QFont f = p.font();
    f.setPixelSize(11);
    p.setFont(f);
    for (const auto &r : rs) {
        QRect hr(0, r.y, headerWidth(), r.h);
        p.fillRect(hr.adjusted(0, 0, 0, -1), QColor(40, 40, 46));
        if (r.kind == Row::Caption) {
            p.setPen(QColor(200, 200, 210));
            p.drawText(hr.adjusted(8, 0, 0, 0), Qt::AlignVCenter, tr("C%1  %2").arg(r.index + 1).arg(seq->captionTracks[r.index].name));
            continue;
        }
        const Track &t = seq->tracks(r.kind == Row::Video ? TrackType::Video : TrackType::Audio)[r.index];
        const QString label = (r.kind == Row::Video ? "V" : "A") + QString::number(r.index + 1);
        QRect target(4, r.y + 6, 26, 18);
        p.setPen(Qt::NoPen);
        p.setBrush(t.targeted ? accentColor() : QColor(60, 60, 66));
        p.drawRoundedRect(target, 3, 3);
        p.setPen(Qt::white);
        p.drawText(target, Qt::AlignCenter, label);
        int x = 36;
        auto toggle = [&](const QString &icon, bool on, QColor onColor) {
            QRect b(x, r.y + 6, 18, 18);
            p.setPen(Qt::NoPen);
            p.setBrush(on ? onColor : QColor(52, 52, 58));
            p.drawRoundedRect(b, 3, 3);
            Icons::get(icon).paint(&p, b.adjusted(2, 2, -2, -2));
            x += 22;
        };
        toggle("lock", t.locked, QColor(160, 90, 60));
        if (r.kind == Row::Video) {
            toggle("eye", !t.muted, QColor(52, 52, 58));
        } else {
            QRect bm(x, r.y + 6, 18, 18);
            p.setPen(Qt::NoPen);
            p.setBrush(t.muted ? QColor(200, 90, 90) : QColor(52, 52, 58));
            p.drawRoundedRect(bm, 3, 3);
            p.setPen(Qt::white);
            p.drawText(bm, Qt::AlignCenter, "M");
            x += 22;
            QRect bs(x, r.y + 6, 18, 18);
            p.setPen(Qt::NoPen);
            p.setBrush(t.solo ? QColor(220, 190, 60) : QColor(52, 52, 58));
            p.drawRoundedRect(bs, 3, 3);
            p.setPen(t.solo ? Qt::black : Qt::white);
            p.drawText(bs, Qt::AlignCenter, "S");
            x += 22;
        }
        toggle("link", t.syncLock, QColor(60, 90, 140));
        p.setPen(QColor(190, 190, 200));
        p.drawText(QRect(x + 2, r.y + 6, headerWidth() - x - 4, 18), Qt::AlignVCenter | Qt::AlignLeft,
                   p.fontMetrics().elidedText(t.name, Qt::ElideRight, headerWidth() - x - 6));
    }
}

void TimelineView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(26, 26, 30));
    Session *s = Session::instance();
    const Sequence *seq = viewSequence();
    if (!seq)
        return;
    auto rs = rows();
    const QSet<QString> &sel = s->selection();
    p.setClipRect(QRect(headerWidth(), rulerHeight(), width() - headerWidth(), height() - rulerHeight()));
    for (const auto &r : rs) {
        if (r.y + r.h < rulerHeight() || r.y > height())
            continue;
        p.fillRect(QRect(headerWidth(), r.y, width(), r.h - 1), r.kind == Row::Audio ? QColor(30, 33, 32) : r.kind == Row::Video ? QColor(31, 31, 36) : QColor(34, 30, 38));
        if (r.kind == Row::Caption) {
            const auto &ct = seq->captionTracks[r.index];
            for (const auto &c : ct.captions) {
                QRectF cr(ticksToX(c.start), r.y + 2, ticksToX(c.end) - ticksToX(c.start), r.h - 4);
                if (cr.right() < headerWidth() || cr.left() > width())
                    continue;
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(150, 110, 60));
                p.drawRoundedRect(cr, 2, 2);
                p.setPen(Qt::white);
                QFont f = p.font();
                f.setPixelSize(10);
                p.setFont(f);
                p.drawText(cr.adjusted(3, 0, -2, 0), Qt::AlignVCenter, p.fontMetrics().elidedText(c.text, Qt::ElideRight, int(cr.width()) - 4));
            }
            continue;
        }
        const Track &t = seq->tracks(r.kind == Row::Video ? TrackType::Video : TrackType::Audio)[r.index];
        for (const auto &c : t.clips)
            paintClip(p, r, t, c, sel.contains(c.id));
        for (const auto &tr : t.transitions) {
            TimeRange rg = edit::transitionRange(t, tr);
            QRectF trr(ticksToX(rg.start), r.y + 2, ticksToX(rg.end) - ticksToX(rg.start), r.h / 2 + 4);
            bool tsel = s->selectedTransition() == tr.id;
            p.setPen(QPen(tsel ? Qt::white : QColor(220, 220, 230), tsel ? 2 : 1));
            p.setBrush(QBrush(QColor(170, 140, 230, 200), Qt::SolidPattern));
            p.drawRoundedRect(trr, 3, 3);
            p.setBrush(QBrush(QColor(90, 60, 150, 160), Qt::BDiagPattern));
            p.drawRoundedRect(trr, 3, 3);
            if (trr.width() > 40) {
                const EffectDef *d = EffectRegistry::instance().find(tr.effect.type);
                p.setPen(Qt::white);
                p.drawText(trr.adjusted(3, 0, -3, 0), Qt::AlignCenter, p.fontMetrics().elidedText(d ? trFx(d->name) : tr.effect.type, Qt::ElideRight, int(trr.width()) - 6));
            }
        }
        if (t.locked)
            p.fillRect(QRect(headerWidth(), r.y, width(), r.h - 1), QBrush(QColor(0, 0, 0, 60), Qt::BDiagPattern));
    }
    // Drop ghost.
    if (m_dropRow >= 0 && m_dropRow < (int)rs.size() && m_dropTime != kInvalidTicks) {
        const Row &r = rs[m_dropRow];
        QRectF g(ticksToX(m_dropTime), r.y + 1, std::max(4.0, m_dropDuration / double(kTicksPerSecond) * m_pps), r.h - 2);
        p.setPen(QPen(Qt::white, 1, Qt::DashLine));
        p.setBrush(QColor(255, 255, 255, 40));
        p.drawRect(g);
    }
    if (m_drag == Drag::Marquee) {
        p.setPen(QPen(QColor(255, 255, 255, 180), 1, Qt::DashLine));
        p.setBrush(QColor(74, 144, 255, 40));
        p.drawRect(m_marquee);
    }
    if (m_snapLine != kInvalidTicks) {
        p.setPen(QPen(QColor(255, 220, 0), 1));
        p.drawLine(QPointF(ticksToX(m_snapLine), rulerHeight()), QPointF(ticksToX(m_snapLine), height()));
    }
    p.setClipping(false);
    paintHeaders(p, rs);
    p.fillRect(QRect(0, 0, headerWidth(), rulerHeight()), QColor(30, 30, 34));
    p.setPen(QColor(74, 144, 255));
    QFont tf = p.font();
    tf.setPixelSize(14);
    tf.setBold(true);
    p.setFont(tf);
    p.drawText(QRect(8, 0, headerWidth() - 8, rulerHeight()), Qt::AlignVCenter, formatTimecode(seq->playhead + seq->startTimecode, seq->fps));
    paintRuler(p);
    // Playhead.
    const double px = ticksToX(seq->playhead);
    if (px >= headerWidth()) {
        p.setPen(QPen(QColor(74, 144, 255), 1));
        p.drawLine(QPointF(px, 0), QPointF(px, height()));
        p.setBrush(QColor(74, 144, 255));
        p.setPen(Qt::NoPen);
        p.drawPolygon(QPolygonF({QPointF(px - 6, 0), QPointF(px + 6, 0), QPointF(px + 6, 10), QPointF(px, 16), QPointF(px - 6, 10)}));
    }
}

// ---------------------------------------------------------------------------
// Interaction

void TimelineView::headerClick(const Row &r, QPoint pos, Qt::MouseButton b)
{
    Session *s = Session::instance();
    if (r.kind == Row::Caption || b != Qt::LeftButton)
        return;
    const TrackType type = r.kind == Row::Video ? TrackType::Video : TrackType::Audio;
    const int idx = r.index;
    const int x = pos.x();
    QString what;
    if (x >= 4 && x < 30) what = "target";
    else if (x >= 36 && x < 54) what = "lock";
    else if (x >= 58 && x < 76) what = type == TrackType::Video ? "eye" : "mute";
    else if (x >= 80 && x < 98) what = type == TrackType::Video ? "sync" : "solo";
    else if (x >= 102 && x < 120 && type == TrackType::Audio) what = "sync";
    if (what.isEmpty())
        return;
    s->doc().editSequence(tr("Toggle Track"), [&](Sequence &seq) {
        Track *t = seq.track(type, idx);
        if (!t) return false;
        if (what == "target") t->targeted = !t->targeted;
        else if (what == "lock") t->locked = !t->locked;
        else if (what == "eye" || what == "mute") t->muted = !t->muted;
        else if (what == "solo") t->solo = !t->solo;
        else if (what == "sync") t->syncLock = !t->syncLock;
        return true;
    });
}

void TimelineView::mousePressEvent(QMouseEvent *e)
{
    Session *s = Session::instance();
    Sequence *seq = s->sequence();
    if (!seq)
        return;
    setFocus();
    s->setFocus(Session::Focus::Timeline);
    m_pressPos = e->pos();
    m_pressHit = hitTest(e->pos());
    m_pressTime = xToTicks(e->pos().x());
    m_snapLine = kInvalidTicks;
    auto rs = rows();
    if (e->button() == Qt::MiddleButton || s->tool() == Tool::Hand) {
        m_drag = Drag::Hand;
        m_pressValue = double(m_scroll);
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    const Hit &h = m_pressHit;
    if (h.part == Hit::Header && h.row >= 0) {
        headerClick(rs[h.row], e->pos(), e->button());
        return;
    }
    if (h.part == Hit::Ruler) {
        m_drag = Drag::Scrub;
        s->setPlayhead(snap(m_pressTime, {}));
        return;
    }
    if (s->tool() == Tool::Zoom) {
        if (e->modifiers() & Qt::AltModifier) zoomOut(); else zoomIn();
        return;
    }
    const Tool tool = s->tool();
    if (h.part == Hit::Transition || h.part == Hit::TransitionEdge) {
        s->selectTransition(h.transitionId);
        if (h.part == Hit::TransitionEdge) {
            m_drag = Drag::TransitionDur;
            m_origin = *seq;
        }
        return;
    }
    if (h.captionIndex >= 0 && h.row >= 0) {
        m_origin = *seq;
        m_drag = h.part == Hit::Caption ? Drag::CaptionMove : Drag::CaptionTrim;
        s->clearSelection();
        return;
    }
    if (h.clipId.isEmpty()) {
        if (tool == Tool::TrackSelectForward || tool == Tool::TrackSelectBackward) {
            QSet<QString> ids;
            const bool fwd = tool == Tool::TrackSelectForward;
            for (const auto *v : {&seq->videoTracks, &seq->audioTracks})
                for (const auto &t : *v)
                    for (const auto &c : t.clips)
                        if (fwd ? c.end() > m_pressTime : c.start < m_pressTime)
                            ids.insert(c.id);
            s->select(ids);
            m_origin = *seq;
            m_drag = Drag::Move;
            return;
        }
        if (!(e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier)))
            s->clearSelection();
        m_drag = Drag::Marquee;
        m_marquee = QRect(e->pos(), QSize());
        return;
    }
    const Clip *clip = seq->clip(h.clipId);
    if (!clip)
        return;
    if (tool == Tool::Razor) {
        Ticks t = snap(m_pressTime, {});
        s->doc().editSequence(tr("Razor"), [&](Sequence &sq) {
            if (e->modifiers() & Qt::ShiftModifier)
                return edit::razor(sq, t) > 0;
            return edit::razorClips(sq, (e->modifiers() & Qt::AltModifier) ? std::vector<QString>{h.clipId} : sq.linkedClips(h.clipId), t) > 0;
        });
        return;
    }
    if (tool == Tool::Pen && h.row >= 0) {
        // Add a keyframe on volume (audio) or opacity (video) at the click.
        const bool audio = rs[h.row].kind == Row::Audio;
        const QString id = h.clipId;
        const Ticks local = m_pressTime - clip->start;
        QRect cr = clipRect(rs[h.row], *clip);
        double fy = std::clamp(double(e->pos().y() - cr.top() - 14) / std::max(1, cr.height() - 16), 0.0, 1.0);
        s->doc().editSequence(tr("Add Keyframe"), [&](Sequence &sq) {
            Clip *c = sq.clip(id);
            if (!c) return false;
            if (audio) {
                AnimatedParam &pp = c->effect("volume")->param("level");
                if (!pp.animated()) pp.setAnimated(true, local);
                pp.addKey({local, Value((1.0 - fy) * 75.0 - 60.0), Interp::Linear});
            } else {
                AnimatedParam &pp = c->effect("opacity")->param("opacity");
                if (!pp.animated()) pp.setAnimated(true, local);
                pp.addKey({local, Value((1.0 - fy) * 100.0), Interp::Linear});
            }
            return true;
        });
        return;
    }
    // Selection.
    if (e->modifiers() & Qt::ControlModifier && tool == Tool::Selection && h.part == Hit::ClipBody) {
        // Ctrl-click toggles (Ctrl-drag = insert move, handled below).
    }
    if (!s->selection().contains(h.clipId)) {
        if (e->modifiers() & Qt::ShiftModifier) {
            QSet<QString> sel = s->selection();
            for (const auto &id : seq->linkedClips(h.clipId)) sel.insert(id);
            s->select(sel);
        } else {
            s->select({h.clipId}, !(e->modifiers() & Qt::AltModifier));
        }
    } else if (e->modifiers() & Qt::ShiftModifier) {
        QSet<QString> sel = s->selection();
        for (const auto &id : seq->linkedClips(h.clipId)) sel.remove(id);
        s->select(sel, false);
        return;
    }
    m_origin = *seq;
    m_trimClip = h.clipId;
    switch (h.part) {
    case Hit::ClipIn:
    case Hit::ClipOut:
        if (tool == Tool::Rolling) m_drag = Drag::Roll;
        else if (tool == Tool::RateStretch) m_drag = Drag::Stretch;
        else m_drag = h.part == Hit::ClipIn ? Drag::TrimIn : Drag::TrimOut;
        break;
    case Hit::VolumeLine: {
        m_drag = Drag::Volume;
        const EffectInstance *v = clip->effect("volume");
        m_pressValue = v ? v->f("level", m_pressTime - clip->start) : 0;
        break;
    }
    default:
        if (tool == Tool::Slip) m_drag = Drag::Slip;
        else if (tool == Tool::Slide) m_drag = Drag::Slide;
        else m_drag = Drag::Move;
        break;
    }
}

void TimelineView::applyDrag(QPointF pos, bool commit)
{
    Session *s = Session::instance();
    if (!m_origin)
        return;
    const Ticks now = xToTicks(pos.x());
    Ticks dt = now - m_pressTime;
    const FrameRate fps = m_origin->fps;
    Sequence preview = *m_origin;
    QString label;
    bool ok = false;
    m_snapLine = kInvalidTicks;
    const auto len = s->sourceLength();
    const Qt::KeyboardModifiers mods = QApplication::keyboardModifiers();
    std::vector<QString> sel(s->selection().begin(), s->selection().end());
    std::set<QString> selSet(sel.begin(), sel.end());
    switch (m_drag) {
    case Drag::Move: {
        if (sel.empty()) break;
        // Snap the selection's earliest start or latest end.
        Ticks first = INT64_MAX, last = 0;
        for (const auto &id : sel)
            if (const Clip *c = m_origin->clip(id)) { first = std::min(first, c->start); last = std::max(last, c->end()); }
        bool sn = false;
        Ticks ns = snap(first + dt, selSet, &sn);
        if (sn) { dt = ns - first; m_snapLine = ns; }
        else {
            Ticks ne = snap(last + dt, selSet, &sn);
            if (sn) { dt = ne - last; m_snapLine = ne; }
        }
        dt = floorToFrame(dt + fps.frameTicks() / 2, fps);
        if (first + dt < 0) dt = -first;
        // Track offset from the vertical movement.
        auto rs = rows();
        const Row *pr = m_pressHit.row >= 0 && m_pressHit.row < (int)rs.size() ? &rs[m_pressHit.row] : nullptr;
        const Row *cr = nullptr;
        for (const auto &r : rs)
            if (pos.y() >= r.y && pos.y() < r.y + r.h) cr = &r;
        int dv = 0, da = 0;
        if (pr && cr && pr->kind == cr->kind) {
            if (pr->kind == Row::Video) dv = cr->index - pr->index;
            if (pr->kind == Row::Audio) da = cr->index - pr->index;
            if (s->linkedSelection()) { if (pr->kind == Row::Video) da = dv; else dv = da; }
        }
        // Clamp so every clip stays on an existing track.
        for (const auto &id : sel)
            if (auto r = m_origin->findClip(id)) {
                int n = (int)m_origin->tracks(r->type).size();
                int d = r->type == TrackType::Video ? dv : da;
                if (r->track + d < 0 || r->track + d >= n) { if (r->type == TrackType::Video) dv = 0; else da = 0; }
            }
        if (mods & Qt::AltModifier) {
            ok = !edit::duplicateClips(preview, sel, dt, dv, da).empty();
            label = tr("Duplicate Clips");
        } else {
            ok = edit::moveClips(preview, sel, dt, dv, da, mods & Qt::ControlModifier);
            label = tr("Move Clips");
        }
        break;
    }
    case Drag::TrimIn:
    case Drag::TrimOut: {
        const Clip *c = m_origin->clip(m_trimClip);
        if (!c) break;
        const bool in = m_drag == Drag::TrimIn;
        Ticks edge = in ? c->start : c->end();
        bool sn = false;
        Ticks target = snap(edge + dt, {m_trimClip}, &sn);
        if (sn) m_snapLine = target;
        target = floorToFrame(target + fps.frameTicks() / 2, fps);
        const edit::TrimMode mode = (s->tool() == Tool::Ripple) ? edit::TrimMode::Ripple : edit::TrimMode::Normal;
        const bool single = mods & Qt::AltModifier;
        if (in) ok = single ? edit::trimIn(preview, m_trimClip, target, mode, len) : edit::trimInLinked(preview, m_trimClip, target, mode, len);
        else ok = single ? edit::trimOut(preview, m_trimClip, target, mode, len) : edit::trimOutLinked(preview, m_trimClip, target, mode, len);
        label = mode == edit::TrimMode::Ripple ? tr("Ripple Trim") : tr("Trim");
        break;
    }
    case Drag::Roll: {
        const Clip *c = m_origin->clip(m_trimClip);
        auto r = m_origin->findClip(m_trimClip);
        if (!c || !r) break;
        const Track &t = m_origin->tracks(r->type)[r->track];
        QString a, b;
        if (m_pressHit.part == Hit::ClipOut) {
            a = c->id;
            for (const auto &o : t.clips) if (o.start == c->end()) b = o.id;
        } else {
            b = c->id;
            for (const auto &o : t.clips) if (o.end() == c->start) a = o.id;
        }
        if (a.isEmpty() || b.isEmpty()) break;
        Ticks cut = m_origin->clip(a)->end();
        Ticks target = floorToFrame(snap(cut + dt, {a, b}) + fps.frameTicks() / 2, fps);
        ok = edit::roll(preview, a, b, target, len);
        // Roll linked partners too.
        if (ok && s->linkedSelection() && !(mods & Qt::AltModifier)) {
            for (const auto &la : m_origin->linkedClips(a)) {
                if (la == a) continue;
                auto lr = m_origin->findClip(la);
                if (!lr) continue;
                for (const auto &o : m_origin->tracks(lr->type)[lr->track].clips)
                    if (o.start == m_origin->clip(la)->end() && m_origin->clip(la)->end() == cut)
                        edit::roll(preview, la, o.id, target, len);
            }
        }
        label = tr("Rolling Edit");
        break;
    }
    case Drag::Slip: {
        const Clip *c = m_origin->clip(m_trimClip);
        if (!c) break;
        Ticks d = -floorToFrame(dt, fps);
        ok = true;
        for (const auto &id : m_origin->linkedClips(m_trimClip))
            ok = edit::slip(preview, id, Ticks(double(d) * c->speed), len) && ok;
        label = tr("Slip");
        break;
    }
    case Drag::Slide: {
        Ticks d = floorToFrame(dt, fps);
        ok = true;
        for (const auto &id : m_origin->linkedClips(m_trimClip))
            ok = edit::slide(preview, id, d, len) && ok;
        label = tr("Slide");
        break;
    }
    case Drag::Stretch: {
        const Clip *c = m_origin->clip(m_trimClip);
        if (!c) break;
        const bool head = m_pressHit.part == Hit::ClipIn;
        Ticks nd = floorToFrame(head ? c->duration - dt : c->duration + dt, fps);
        ok = true;
        for (const auto &id : m_origin->linkedClips(m_trimClip))
            ok = edit::rateStretch(preview, id, nd, head) && ok;
        label = tr("Rate Stretch");
        break;
    }
    case Drag::Volume: {
        const double ddb = -(pos.y() - m_pressPos.y()) * 0.25;
        for (const auto &id : sel) {
            Clip *c = preview.clip(id);
            if (!c) continue;
            EffectInstance *v = c->effect("volume");
            if (!v) continue;
            AnimatedParam &pp = v->param("level");
            if (pp.animated()) {
                for (auto &k : pp.keys()) k.value.v[0] = std::clamp(m_origin->clip(id)->effect("volume")->params.at("level").keys()[&k - &pp.keys()[0]].value.f() + ddb, -96.0, 15.0);
            } else {
                double base = m_origin->clip(id)->effect("volume")->f("level", 0);
                pp.setStaticValue(Value(std::clamp(base + ddb, -96.0, 15.0)));
            }
        }
        ok = true;
        label = tr("Adjust Volume");
        emit Session::instance()->statusMessage(tr("Volume %1 dB").arg(m_pressValue + ddb, 0, 'f', 1));
        break;
    }
    case Drag::TransitionDur: {
        for (auto *v : {&preview.videoTracks, &preview.audioTracks})
            for (auto &t : *v)
                if (Transition *tr = t.transition(m_pressHit.transitionId)) {
                    Ticks d = m_pressHit.clipId == "<" ? -dt : dt;
                    if (tr->alignment == Transition::CenterAtCut) d *= 2;
                    tr->duration = std::max(fps.frameTicks(), floorToFrame(tr->duration + d, fps));
                }
        edit::normalize(preview);
        ok = true;
        label = tr("Transition Duration");
        break;
    }
    case Drag::CaptionMove:
    case Drag::CaptionTrim: {
        auto rs = rows();
        if (m_pressHit.row < 0 || m_pressHit.row >= (int)rs.size()) break;
        auto &ct = preview.captionTracks[rs[m_pressHit.row].index];
        if (m_pressHit.captionIndex >= (int)ct.captions.size()) break;
        Caption &c = ct.captions[m_pressHit.captionIndex];
        Ticks d = floorToFrame(dt, fps);
        if (m_drag == Drag::CaptionMove) { c.start = std::max<Ticks>(0, c.start + d); c.end = c.start + (m_origin->captionTracks[rs[m_pressHit.row].index].captions[m_pressHit.captionIndex].end - m_origin->captionTracks[rs[m_pressHit.row].index].captions[m_pressHit.captionIndex].start); }
        else if (m_pressHit.part == Hit::CaptionIn) c.start = std::min(c.end - fps.frameTicks(), c.start + d);
        else c.end = std::max(c.start + fps.frameTicks(), c.end + d);
        ct.sort();
        ok = true;
        label = tr("Edit Caption Timing");
        break;
    }
    default:
        break;
    }
    if (ok)
        m_preview = preview;
    else if (!commit)
        m_preview = *m_origin;
    if (commit) {
        if (ok && m_preview) {
            Sequence result = *m_preview;
            s->doc().editSequence(label, [&](Sequence &sq) {
                Ticks ph = sq.playhead;
                sq = result;
                sq.playhead = ph;
                return true;
            });
        }
        m_preview.reset();
        m_origin.reset();
    }
    update();
}

void TimelineView::mouseMoveEvent(QMouseEvent *e)
{
    Session *s = Session::instance();
    if (m_drag == Drag::None) {
        Hit h = hitTest(e->pos());
        Qt::CursorShape cur = Qt::ArrowCursor;
        const Tool tool = s->tool();
        if (tool == Tool::Razor) cur = Qt::CrossCursor;
        else if (tool == Tool::Hand) cur = Qt::OpenHandCursor;
        else if (h.part == Hit::ClipIn || h.part == Hit::ClipOut || h.part == Hit::TransitionEdge || h.part == Hit::CaptionIn || h.part == Hit::CaptionOut)
            cur = Qt::SizeHorCursor;
        else if (h.part == Hit::VolumeLine) cur = Qt::SizeVerCursor;
        setCursor(cur);
        return;
    }
    if (m_drag == Drag::Hand) {
        m_scroll = std::max<Ticks>(0, Ticks(m_pressValue - (e->pos().x() - m_pressPos.x()) / m_pps * kTicksPerSecond));
        updateScrollbars();
        update();
        return;
    }
    if (m_drag == Drag::Scrub) {
        bool sn = false;
        Ticks t = snap(xToTicks(e->pos().x()), {}, &sn);
        s->setPlayhead(std::max<Ticks>(0, t));
        return;
    }
    if (m_drag == Drag::Marquee) {
        m_marquee = QRect(m_pressPos, e->pos()).normalized();
        const Sequence *seq = s->sequence();
        QSet<QString> ids;
        auto rs = rows();
        for (const auto &r : rs) {
            if (r.kind == Row::Caption) continue;
            const Track &t = seq->tracks(r.kind == Row::Video ? TrackType::Video : TrackType::Audio)[r.index];
            for (const auto &c : t.clips)
                if (clipRect(r, c).intersects(m_marquee))
                    ids.insert(c.id);
        }
        if (e->modifiers() & Qt::ShiftModifier)
            ids.unite(s->selection());
        s->select(ids);
        update();
        return;
    }
    if ((e->pos() - m_pressPos).manhattanLength() < 3 && !m_preview)
        return;
    applyDrag(e->position(), false);
}

void TimelineView::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_drag != Drag::None && m_drag != Drag::Scrub && m_drag != Drag::Marquee && m_drag != Drag::Hand) {
        if ((e->pos() - m_pressPos).manhattanLength() >= 3 || m_preview)
            applyDrag(e->position(), true);
        m_origin.reset();
        m_preview.reset();
    }
    m_drag = Drag::None;
    m_snapLine = kInvalidTicks;
    update();
}

void TimelineView::mouseDoubleClickEvent(QMouseEvent *e)
{
    Session *s = Session::instance();
    Hit h = hitTest(e->pos());
    Sequence *seq = s->sequence();
    if (!seq)
        return;
    if (h.captionIndex >= 0 && h.row >= 0) {
        auto rs = rows();
        const int ti = rs[h.row].index;
        QString text = QInputDialog::getMultiLineText(this, tr("Edit Caption"), tr("Text:"), seq->captionTracks[ti].captions[h.captionIndex].text);
        if (!text.isNull())
            s->doc().editSequence(tr("Edit Caption"), [&](Sequence &sq) { sq.captionTracks[ti].captions[h.captionIndex].text = text; return true; });
        return;
    }
    const Clip *c = seq->clip(h.clipId);
    if (!c)
        return;
    if (c->source == ClipSource::Sequence)
        emit openNested(c->mediaId);
    else if (c->source == ClipSource::Media) {
        s->setSourceMedia(c->mediaId);
        s->setSourcePosition(c->sourceAt(std::clamp(seq->playhead, c->start, c->end() - 1)));
    }
}

void TimelineView::wheelEvent(QWheelEvent *e)
{
    const int d = e->angleDelta().y() != 0 ? e->angleDelta().y() : e->angleDelta().x();
    if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) {
        const Ticks anchor = xToTicks(e->position().x());
        const double f = d > 0 ? 1.25 : 0.8;
        m_pps = std::clamp(m_pps * f, 0.02, 4000.0);
        m_scroll = std::max<Ticks>(0, anchor - Ticks((e->position().x() - headerWidth()) / m_pps * kTicksPerSecond));
    } else if (e->modifiers() & Qt::ShiftModifier) {
        m_vbar->setValue(m_vbar->value() - d / 2);
    } else {
        m_scroll = std::max<Ticks>(0, m_scroll - Ticks(d / 120.0 * 60.0 / m_pps * kTicksPerSecond));
    }
    updateScrollbars();
    update();
}

void TimelineView::clipMenu(const QString &clipId, QPoint global, Ticks at)
{
    Session *s = Session::instance();
    QMenu m(this);
    auto act = [&](const QString &text, const QString &action) {
        QAction *a = m.addAction(text);
        connect(a, &QAction::triggered, this, [this, action] { emit requestClipMenuAction(action); });
        return a;
    };
    act(tr("Cut"), "cut");
    act(tr("Copy"), "copy");
    act(tr("Paste Attributes..."), "pasteAttributes");
    m.addSeparator();
    act(tr("Clear"), "delete");
    act(tr("Ripple Delete"), "rippleDelete");
    m.addSeparator();
    act(tr("Enable / Disable"), "toggleEnable");
    act(tr("Unlink"), "unlink");
    act(tr("Link"), "link");
    act(tr("Group"), "group");
    act(tr("Ungroup"), "ungroup");
    m.addSeparator();
    act(tr("Speed / Duration..."), "speed");
    act(tr("Audio Gain..."), "audioGain");
    act(tr("Normalize Loudness"), "normalize");
    m.addSeparator();
    act(tr("Set to Frame Size (Fit)"), "fit");
    act(tr("Scale to Fill"), "fill");
    act(tr("Auto Reframe (vertical)"), "autoReframe");
    act(tr("Scene Edit Detection"), "sceneDetect");
    act(tr("Remove Silences"), "removeSilence");
    act(tr("Transcribe to Captions"), "transcribe");
    act(tr("Warp Stabilizer"), "stabilize");
    m.addSeparator();
    act(tr("Nest..."), "nest");
    act(tr("Make Subclip"), "subclip");
    act(tr("Rename..."), "rename");
    act(tr("Reveal in Project"), "reveal");
    QMenu *labels = m.addMenu(tr("Label"));
    for (int i = 0; i < (int)labelColors().size(); ++i) {
        QPixmap px(12, 12);
        px.fill(labelColors()[i]);
        QAction *a = labels->addAction(QIcon(px), labelColorNames()[i]);
        connect(a, &QAction::triggered, this, [s, i] {
            QSet<QString> sel = s->selection();
            s->doc().editSequence(tr("Label"), [&](Sequence &sq) {
                for (const auto &id : sel) if (Clip *c = sq.clip(id)) c->label = i;
                return true;
            });
        });
    }
    (void)clipId;
    (void)at;
    m.exec(global);
}

void TimelineView::contextMenuEvent(QContextMenuEvent *e)
{
    Session *s = Session::instance();
    Sequence *seq = s->sequence();
    if (!seq)
        return;
    Hit h = hitTest(e->pos());
    auto rs = rows();
    if (h.part == Hit::Header && h.row >= 0) {
        const Row r = rs[h.row];
        QMenu m(this);
        if (r.kind != Row::Caption) {
            const TrackType type = r.kind == Row::Video ? TrackType::Video : TrackType::Audio;
            m.addAction(tr("Add Track"), this, [s, type] { s->doc().editSequence(tr("Add Track"), [&](Sequence &sq) { sq.addTrack(type); return true; }); });
            m.addAction(tr("Delete Track"), this, [s, type, r] {
                s->doc().editSequence(tr("Delete Track"), [&](Sequence &sq) {
                    auto &v = sq.tracks(type);
                    if (v.size() <= 1) return false;
                    v.erase(v.begin() + r.index);
                    sq.renameTracks();
                    return true;
                });
            });
            m.addAction(tr("Rename Track..."), this, [this, s, type, r] {
                QString n = QInputDialog::getText(this, tr("Rename Track"), tr("Name:"), QLineEdit::Normal, s->sequence()->tracks(type)[r.index].name);
                if (!n.isEmpty())
                    s->doc().editSequence(tr("Rename Track"), [&](Sequence &sq) { sq.tracks(type)[r.index].name = n; return true; });
            });
            m.addAction(tr("Taller"), this, [s, type, r] { s->doc().editSequence(tr("Track Height"), [&](Sequence &sq) { auto &t = sq.tracks(type)[r.index]; t.height = std::min(220, (t.height ? t.height : kTrackH) + 24); return true; }); });
            m.addAction(tr("Shorter"), this, [s, type, r] { s->doc().editSequence(tr("Track Height"), [&](Sequence &sq) { auto &t = sq.tracks(type)[r.index]; t.height = std::max(28, (t.height ? t.height : kTrackH) - 24); return true; }); });
        } else {
            m.addAction(tr("Delete Caption Track"), this, [s, r] { s->doc().editSequence(tr("Delete Caption Track"), [&](Sequence &sq) { sq.captionTracks.erase(sq.captionTracks.begin() + r.index); return true; }); });
        }
        m.exec(e->globalPos());
        return;
    }
    if (!h.transitionId.isEmpty()) {
        QMenu m(this);
        const QString id = h.transitionId;
        m.addAction(tr("Set Duration..."), this, [this, s, id] {
            bool ok = false;
            double sec = QInputDialog::getDouble(this, tr("Transition Duration"), tr("Seconds:"), 1.0, 0.04, 60, 2, &ok);
            if (ok)
                s->doc().editSequence(tr("Transition Duration"), [&](Sequence &sq) {
                    for (auto *v : {&sq.videoTracks, &sq.audioTracks})
                        for (auto &t : *v)
                            if (Transition *tr = t.transition(id)) tr->duration = floorToFrame(secondsToTicks(sec), sq.fps);
                    edit::normalize(sq);
                    return true;
                });
        });
        m.addAction(tr("Delete"), this, [s, id] {
            s->doc().editSequence(tr("Delete Transition"), [&](Sequence &sq) {
                for (auto *v : {&sq.videoTracks, &sq.audioTracks})
                    for (auto &t : *v)
                        t.transitions.erase(std::remove_if(t.transitions.begin(), t.transitions.end(), [&](const Transition &x) { return x.id == id; }), t.transitions.end());
                return true;
            });
        });
        m.exec(e->globalPos());
        return;
    }
    if (!h.clipId.isEmpty()) {
        if (!s->selection().contains(h.clipId))
            s->select({h.clipId});
        clipMenu(h.clipId, e->globalPos(), h.time);
        return;
    }
    if (h.part == Hit::Empty && h.row >= 0 && rs[h.row].kind != Row::Caption) {
        const Row r = rs[h.row];
        const TrackType type = r.kind == Row::Video ? TrackType::Video : TrackType::Audio;
        const Ticks t = h.time;
        QMenu m(this);
        m.addAction(tr("Ripple Delete Gap"), this, [s, type, r, t] {
            s->doc().editSequence(tr("Ripple Delete"), [&](Sequence &sq) { return edit::closeGap(sq, {type, r.index}, t); });
        });
        m.addAction(tr("Close All Gaps"), this, [s] { s->doc().editSequence(tr("Close Gaps"), [&](Sequence &sq) { edit::closeAllGaps(sq); return true; }); });
        m.addAction(tr("Paste"), this, [this] { emit requestClipMenuAction("paste"); });
        m.exec(e->globalPos());
    }
}

void TimelineView::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasFormat(kMediaMime) || e->mimeData()->hasFormat(kEffectMime) || e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void TimelineView::dragMoveEvent(QDragMoveEvent *e)
{
    Session *s = Session::instance();
    auto rs = rows();
    m_dropRow = -1;
    for (int i = 0; i < (int)rs.size(); ++i)
        if (e->position().y() >= rs[i].y && e->position().y() < rs[i].y + rs[i].h && rs[i].kind != Row::Caption)
            m_dropRow = i;
    if (e->mimeData()->hasFormat(kMediaMime)) {
        QStringList ids = QString::fromUtf8(e->mimeData()->data(kMediaMime)).split('\n', Qt::SkipEmptyParts);
        m_dropDuration = 0;
        for (const auto &id : ids)
            if (const MediaItem *m = s->project().mediaItem(id))
                m_dropDuration += m->info.isStill ? s->project().settings.defaultStillDuration
                                                  : (m->outPoint != kInvalidTicks ? m->outPoint : m->info.duration) - (m->inPoint != kInvalidTicks ? m->inPoint : 0);
        m_dropTime = std::max<Ticks>(0, snap(xToTicks(e->position().x()), {}));
    } else {
        m_dropTime = kInvalidTicks;
    }
    e->acceptProposedAction();
    update();
}

void TimelineView::dragLeaveEvent(QDragLeaveEvent *)
{
    m_dropRow = -1;
    m_dropTime = kInvalidTicks;
    update();
}

void TimelineView::dropEvent(QDropEvent *e)
{
    Session *s = Session::instance();
    auto rs = rows();
    const Row *row = (m_dropRow >= 0 && m_dropRow < (int)rs.size()) ? &rs[m_dropRow] : nullptr;
    const Ticks at = std::max<Ticks>(0, snap(xToTicks(e->position().x()), {}));
    int vt = row && row->kind == Row::Video ? row->index : 0;
    int at_ = row && row->kind == Row::Audio ? row->index : 0;
    if (row && row->kind == Row::Video) at_ = std::min(vt, (int)s->sequence()->audioTracks.size() - 1);
    if (row && row->kind == Row::Audio) vt = std::min(at_, (int)s->sequence()->videoTracks.size() - 1);
    const bool insert = e->modifiers() & Qt::ControlModifier;
    m_dropRow = -1;
    m_dropTime = kInvalidTicks;
    if (e->mimeData()->hasFormat(kMediaMime)) {
        QStringList ids = QString::fromUtf8(e->mimeData()->data(kMediaMime)).split('\n', Qt::SkipEmptyParts);
        s->doc().editSequence(insert ? tr("Insert") : tr("Overwrite"), [&](Sequence &seq) {
            Ticks pos = at;
            bool any = false;
            for (const auto &id : ids) {
                const MediaItem *m = s->project().mediaItem(id);
                if (!m)
                    continue;
                s->matchSequenceToMedia(seq, *m);
                auto clips = s->makeClips(*m, pos, vt, at_, !row || row->kind == Row::Video || !m->info.hasAudio, !row || row->kind == Row::Audio || true);
                if (clips.empty())
                    continue;
                Ticks dur = clips.front().second.duration;
                bool ok = insert ? edit::insertEdit(seq, clips, pos, dur) : edit::overwriteEdit(seq, clips);
                any = any || ok;
                pos += dur;
            }
            return any;
        });
    } else if (e->mimeData()->hasFormat(kEffectMime)) {
        const QString type = QString::fromUtf8(e->mimeData()->data(kEffectMime));
        const EffectDef *d = EffectRegistry::instance().find(type);
        Hit h = hitTest(e->position().toPoint());
        if (!d) {
            // Graphics template or generator: place a synthetic clip.
            const int track = row && row->kind == Row::Video ? row->index : 1;
            s->doc().editSequence(tr("Add Graphic"), [&](Sequence &seq) {
                auto c = s->makeGenerator(type, at, seq);
                if (!c) return false;
                while ((int)seq.videoTracks.size() <= track) seq.addTrack(TrackType::Video);
                seq.renameTracks();
                std::vector<std::pair<edit::TrackRef, Clip>> list{{{TrackType::Video, track}, *c}};
                return insert ? edit::insertEdit(seq, list, c->start, c->duration) : edit::overwriteEdit(seq, list);
            });
            update();
            return;
        }
        if (d->kind == EffectKind::VideoTransition || d->kind == EffectKind::AudioTransition) {
            const Sequence *seq = s->sequence();
            const Clip *c = seq->clip(h.clipId);
            auto ref = seq->findClip(h.clipId);
            if (!c || !ref)
                return;
            const Track &t = seq->tracks(ref->type)[ref->track];
            const bool nearStart = h.time - c->start < c->end() - h.time;
            QString a, b;
            if (nearStart) {
                b = c->id;
                for (const auto &o : t.clips) if (o.end() == c->start) a = o.id;
            } else {
                a = c->id;
                for (const auto &o : t.clips) if (o.start == c->end()) b = o.id;
            }
            const Ticks dur = d->kind == EffectKind::VideoTransition ? s->project().settings.defaultVideoTransition : s->project().settings.defaultAudioTransition;
            s->doc().editSequence(tr("Add Transition"), [&](Sequence &sq) {
                return !edit::addTransition(sq, {ref->type, ref->track}, a, b, type, floorToFrame(dur, sq.fps)).isEmpty();
            });
        } else {
            QSet<QString> targets = s->selection().contains(h.clipId) ? s->selection() : QSet<QString>{h.clipId};
            s->doc().editSequence(tr("Apply Effect"), [&](Sequence &sq) {
                bool any = false;
                for (const auto &id : targets) {
                    auto r = sq.findClip(id);
                    if (!r) continue;
                    if ((d->kind == EffectKind::Video) != (r->type == TrackType::Video)) continue;
                    sq.tracks(r->type)[r->track].clips[r->index].effects.push_back(EffectInstance::create(type));
                    any = true;
                }
                return any;
            });
            s->select({h.clipId});
        }
    } else if (e->mimeData()->hasUrls()) {
        QStringList paths;
        for (const auto &u : e->mimeData()->urls())
            if (u.isLocalFile())
                paths << u.toLocalFile();
        emit requestImport(paths, at, vt, at_);
    }
    update();
}

void TimelineView::keyPressEvent(QKeyEvent *e)
{
    QWidget::keyPressEvent(e);
}

void TimelineView::resizeEvent(QResizeEvent *)
{
    m_hbar->setGeometry(headerWidth(), height() - 14, width() - headerWidth() - 14, 14);
    m_vbar->setGeometry(width() - 14, rulerHeight(), 14, height() - rulerHeight() - 14);
    updateScrollbars();
}

void TimelineView::updateScrollbars()
{
    const Sequence *seq = Session::instance()->sequence();
    if (!seq)
        return;
    const double visible = (width() - headerWidth()) / m_pps;
    const double total = std::max(ticksToSeconds(seq->duration()) + visible * 0.5, visible);
    m_hbar->blockSignals(true);
    m_hbar->setRange(0, int(std::max(0.0, total - visible) * 100));
    m_hbar->setPageStep(int(visible * 100));
    m_hbar->setValue(int(ticksToSeconds(m_scroll) * 100));
    m_hbar->blockSignals(false);
    auto rs = rows();
    int contentH = rs.empty() ? 0 : rs.back().y + rs.back().h + m_scrollY - trackAreaTop();
    m_vbar->setRange(0, std::max(0, contentH - (height() - rulerHeight() - 14)));
    m_vbar->setPageStep(height());
}

void TimelineView::zoomIn()
{
    const Ticks ph = Session::instance()->playhead();
    m_pps = std::min(4000.0, m_pps * 1.5);
    m_scroll = std::max<Ticks>(0, ph - Ticks((width() - headerWidth()) / 2.0 / m_pps * kTicksPerSecond));
    updateScrollbars();
    update();
}

void TimelineView::zoomOut()
{
    const Ticks ph = Session::instance()->playhead();
    m_pps = std::max(0.02, m_pps / 1.5);
    m_scroll = std::max<Ticks>(0, ph - Ticks((width() - headerWidth()) / 2.0 / m_pps * kTicksPerSecond));
    updateScrollbars();
    update();
}

void TimelineView::zoomToFit()
{
    const Sequence *seq = Session::instance()->sequence();
    double dur = seq ? std::max(10.0, ticksToSeconds(seq->duration()) * 1.05) : 60.0;
    m_pps = std::clamp((width() - headerWidth() - 20) / dur, 0.02, 4000.0);
    m_scroll = 0;
    updateScrollbars();
    update();
}

void TimelineView::ensurePlayheadVisible()
{
    const Ticks ph = Session::instance()->playhead();
    const double x = ticksToX(ph);
    if (x < headerWidth() || x > width() - 20) {
        // Page scroll like Premiere.
        m_scroll = std::max<Ticks>(0, ph - Ticks(20.0 / m_pps * kTicksPerSecond));
        updateScrollbars();
    }
}

// ---------------------------------------------------------------------------

TimelinePanel::TimelinePanel(QWidget *parent) : QWidget(parent)
{
    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    auto *tools = new QVBoxLayout;
    tools->setContentsMargins(2, 2, 2, 2);
    tools->setSpacing(1);
    Session *s = Session::instance();
    struct T { Tool tool; const char *icon; const char *tip; };
    const T list[] = {
        {Tool::Selection, "select", "Selection Tool (V)"}, {Tool::TrackSelectForward, "track-select", "Track Select Forward (A)"},
        {Tool::Ripple, "ripple", "Ripple Edit Tool (B)"}, {Tool::Rolling, "roll", "Rolling Edit Tool (N)"},
        {Tool::RateStretch, "rate", "Rate Stretch Tool (R)"}, {Tool::Razor, "razor", "Razor Tool (C)"},
        {Tool::Slip, "slip", "Slip Tool (Y)"}, {Tool::Slide, "slide", "Slide Tool (U)"}, {Tool::Pen, "pen", "Pen Tool (P)"},
        {Tool::Hand, "hand", "Hand Tool (H)"}, {Tool::Zoom, "zoom", "Zoom Tool (Z)"},
    };
    std::vector<QToolButton *> buttons;
    for (const auto &t : list) {
        auto *b = new QToolButton(this);
        b->setIcon(Icons::get(t.icon));
        b->setToolTip(tr(t.tip));
        b->setCheckable(true);
        b->setAutoRaise(true);
        b->setIconSize(QSize(20, 20));
        b->setChecked(t.tool == Tool::Selection);
        const Tool tool = t.tool;
        connect(b, &QToolButton::clicked, this, [s, tool] { s->setTool(tool); });
        tools->addWidget(b);
        buttons.push_back(b);
    }
    connect(s, &Session::toolChanged, this, [buttons, list](Tool t) {
        for (size_t i = 0; i < buttons.size(); ++i)
            buttons[i]->setChecked(list[i].tool == t);
    });
    tools->addSpacing(10);
    auto *snapBtn = new QToolButton(this);
    snapBtn->setIcon(Icons::get("snap"));
    snapBtn->setToolTip(tr("Snap (S)"));
    snapBtn->setCheckable(true);
    snapBtn->setChecked(true);
    snapBtn->setAutoRaise(true);
    connect(snapBtn, &QToolButton::toggled, s, &Session::setSnapping);
    connect(s, &Session::settingsChanged, snapBtn, [snapBtn, s] { snapBtn->setChecked(s->snapping()); });
    tools->addWidget(snapBtn);
    auto *linkBtn = new QToolButton(this);
    linkBtn->setIcon(Icons::get("link"));
    linkBtn->setToolTip(tr("Linked Selection"));
    linkBtn->setCheckable(true);
    linkBtn->setChecked(true);
    linkBtn->setAutoRaise(true);
    connect(linkBtn, &QToolButton::toggled, s, &Session::setLinkedSelection);
    tools->addWidget(linkBtn);
    tools->addStretch();
    lay->addLayout(tools);
    m_view = new TimelineView(this);
    lay->addWidget(m_view, 1);
}

} // namespace rc
