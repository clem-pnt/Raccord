#include "Session.h"
#include "render/Title.h"

namespace rc {

static Session *s_instance = nullptr;

Session::Session(QObject *parent) : QObject(parent)
{
    s_instance = this;
    connect(&m_doc, &Document::changed, this, [this] {
        m_snapshotDirty = true;
        // Drop selected ids that no longer exist.
        if (Sequence *s = sequence()) {
            QSet<QString> keep;
            for (const auto &id : m_selection)
                if (s->clip(id))
                    keep.insert(id);
            if (keep != m_selection) {
                m_selection = keep;
                if (!keep.contains(m_primary))
                    m_primary = keep.isEmpty() ? QString() : *keep.begin();
                emit selectionChanged();
            }
        }
    });
    connect(&m_doc, &Document::projectReplaced, this, [this] {
        m_selection.clear();
        m_primary.clear();
        m_sourceMedia.clear();
        emit selectionChanged();
        emit sequenceChanged();
        emit sourceChanged();
    });
}

Session *Session::instance() { return s_instance; }

std::shared_ptr<const Project> Session::snapshot()
{
    if (m_snapshotDirty || !m_snapshot) {
        m_snapshot = std::make_shared<const Project>(m_doc.project());
        m_snapshotDirty = false;
    }
    return m_snapshot;
}

void Session::select(const QSet<QString> &idsIn, bool expandLinks)
{
    QSet<QString> ids = idsIn;
    if (Sequence *s = sequence()) {
        if (expandLinks && m_linkedSelection)
            for (const auto &id : idsIn)
                for (const auto &l : s->linkedClips(id))
                    ids.insert(l);
        // Groups always select together.
        for (const auto &id : QSet<QString>(ids)) {
            const Clip *c = s->clip(id);
            if (c && !c->groupId.isEmpty())
                for (const auto *v : {&s->videoTracks, &s->audioTracks})
                    for (const auto &t : *v)
                        for (const auto &o : t.clips)
                            if (o.groupId == c->groupId)
                                ids.insert(o.id);
        }
    }
    m_transition.clear();
    if (ids == m_selection)
        return;
    m_selection = ids;
    m_primary = idsIn.isEmpty() ? QString() : *idsIn.begin();
    emit selectionChanged();
}

void Session::selectTransition(const QString &id)
{
    m_selection.clear();
    m_primary.clear();
    m_transition = id;
    emit selectionChanged();
}

Ticks Session::playhead() const
{
    const Sequence *s = const_cast<Session *>(this)->sequence();
    return s ? s->playhead : 0;
}

void Session::setPlayhead(Ticks t, bool fromPlayback)
{
    Sequence *s = sequence();
    if (!s)
        return;
    t = std::max<Ticks>(0, t);
    if (!fromPlayback)
        t = floorToFrame(t, s->fps);
    if (s->playhead == t)
        return;
    s->playhead = t;
    emit playheadChanged(t);
}

void Session::setSourceMedia(const QString &mediaId)
{
    m_sourceMedia = mediaId;
    m_sourcePos = 0;
    if (const MediaItem *m = project().mediaItem(mediaId))
        if (m->inPoint != kInvalidTicks)
            m_sourcePos = m->inPoint;
    emit sourceChanged();
    emit sourcePositionChanged(m_sourcePos);
}

void Session::setSourcePosition(Ticks t)
{
    m_sourcePos = std::max<Ticks>(0, t);
    emit sourcePositionChanged(m_sourcePos);
}

void Session::setActiveSequence(const QString &id)
{
    if (project().activeSequenceId == id)
        return;
    project().activeSequenceId = id;
    m_selection.clear();
    emit selectionChanged();
    emit sequenceChanged();
    m_doc.touch();
}

edit::SourceLength Session::sourceLength() const
{
    const Project *p = &m_doc.project();
    return [p](const Clip &c) -> Ticks {
        if (c.source == ClipSource::Media) {
            const MediaItem *m = p->mediaItem(c.mediaId);
            if (!m || m->info.isStill)
                return INT64_MAX / 4;
            return m->info.duration;
        }
        if (c.source == ClipSource::Sequence) {
            const Sequence *s = p->sequence(c.mediaId);
            return s ? s->duration() : 0;
        }
        return INT64_MAX / 4;
    };
}

std::vector<edit::TrackRef> Session::targetedTracks() const
{
    std::vector<edit::TrackRef> out;
    const Sequence *s = const_cast<Session *>(this)->sequence();
    if (!s)
        return out;
    for (int i = 0; i < (int)s->videoTracks.size(); ++i)
        if (s->videoTracks[i].targeted && !s->videoTracks[i].locked)
            out.push_back({TrackType::Video, i});
    for (int i = 0; i < (int)s->audioTracks.size(); ++i)
        if (s->audioTracks[i].targeted && !s->audioTracks[i].locked)
            out.push_back({TrackType::Audio, i});
    return out;
}


std::vector<std::pair<edit::TrackRef, Clip>> Session::makeClips(const MediaItem &m, Ticks at, int videoTrack, int audioTrack, bool video, bool audio)
{
    std::vector<std::pair<edit::TrackRef, Clip>> out;
    const Sequence *seq = sequence();
    if (!seq)
        return out;
    Ticks in = m.inPoint != kInvalidTicks ? m.inPoint : 0;
    Ticks end = m.info.isStill ? in + project().settings.defaultStillDuration : m.info.duration;
    if (m.outPoint != kInvalidTicks)
        end = m.outPoint;
    Ticks dur = floorToFrame(std::max<Ticks>(seq->fps.frameTicks(), end - in), seq->fps);
    at = floorToFrame(at, seq->fps);
    const QString link = newId();
    auto mk = [&](TrackType t) {
        Clip c;
        c.id = newId();
        c.name = m.name;
        c.source = ClipSource::Media;
        c.mediaId = m.id;
        c.start = at;
        c.duration = dur;
        c.in = in;
        c.label = m.label;
        c.linkId = (video && audio && m.info.hasVideo && m.info.hasAudio) ? link : QString();
        c.ensureIntrinsics(t);
        return c;
    };
    if (video && m.info.hasVideo)
        out.push_back({{TrackType::Video, std::max(0, videoTrack)}, mk(TrackType::Video)});
    if (audio && m.info.hasAudio)
        out.push_back({{TrackType::Audio, std::max(0, audioTrack)}, mk(TrackType::Audio)});
    return out;
}

void Session::matchSequenceToMedia(Sequence &seq, const MediaItem &m)
{
    if (seq.duration() > 0 || !m.info.hasVideo || m.info.isStill)
        return;
    const bool rot = m.info.rotation % 180 != 0;
    seq.width = (int(std::lround((rot ? m.info.height : m.info.width) * m.info.pixelAspect)) + 1) & ~1;
    seq.height = ((rot ? m.info.width : m.info.height) + 1) & ~1;
    seq.fps = m.fps();
}

std::optional<Clip> Session::makeGenerator(const QString &id, Ticks at, const Sequence &seq)
{
    Clip c;
    c.id = newId();
    c.start = floorToFrame(at, seq.fps);
    c.duration = floorToFrame(project().settings.defaultStillDuration, seq.fps);
    if (id.startsWith("title:")) {
        const QString name = id.mid(6);
        c.source = ClipSource::Title;
        c.name = name;
        c.gen = TitleRenderer::templateDoc(name);
    } else if (id == "gen:solid") {
        c.source = ClipSource::Solid;
        c.name = tr("Color Matte");
        c.gen["color"] = "#202020";
    } else if (id == "gen:adjustment") {
        c.source = ClipSource::Adjustment;
        c.name = tr("Adjustment Layer");
    } else if (id == "gen:bars") {
        c.source = ClipSource::Bars;
        c.name = tr("Bars and Tone");
    } else if (id == "gen:countdown") {
        c.source = ClipSource::Countdown;
        c.name = tr("Universal Counting Leader");
        c.duration = floorToFrame(11 * kTicksPerSecond, seq.fps);
    } else if (id == "gen:transparent") {
        c.source = ClipSource::Transparent;
        c.name = tr("Transparent Video");
    } else {
        return std::nullopt;
    }
    c.ensureIntrinsics(TrackType::Video);
    return c;
}

} // namespace rc
