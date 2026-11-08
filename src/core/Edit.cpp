#include "Edit.h"
#include <algorithm>
#include <climits>
#include <map>

namespace rc::edit {

Ticks unlimited(const Clip &) { return INT64_MAX / 4; }

static constexpr Ticks kMinDur = 1;   // minimum clip duration in ticks

static void regenerateUids(Clip &c)
{
    for (auto &e : c.effects)
        e.uid = newId();
}

static std::vector<TrackRef> allTracks(const Sequence &seq, bool includeLocked = false)
{
    std::vector<TrackRef> out;
    for (int i = 0; i < (int)seq.videoTracks.size(); ++i)
        if (includeLocked || !seq.videoTracks[i].locked)
            out.push_back({TrackType::Video, i});
    for (int i = 0; i < (int)seq.audioTracks.size(); ++i)
        if (includeLocked || !seq.audioTracks[i].locked)
            out.push_back({TrackType::Audio, i});
    return out;
}

// ---------------------------------------------------------------------------
QString splitAt(Track &track, Ticks t)
{
    int idx = track.clipIndexAt(t);
    if (idx < 0)
        return {};
    Clip &c = track.clips[idx];
    if (t <= c.start || t >= c.end())
        return {};
    const Ticks L = t - c.start;
    Clip right = c;
    right.id = newId();
    regenerateUids(right);
    right.start = t;
    right.duration = c.duration - L;
    if (!c.reverse) {
        right.in = c.in + (Ticks)std::llround(double(L) * c.speed);
    } else {
        right.in = c.in;
        c.in = c.in + (Ticks)std::llround(double(c.duration - L) * c.speed);
    }
    for (auto &e : right.effects)
        for (auto &[k, p] : e.params)
            p.shiftKeys(-L);
    c.duration = L;
    // Transitions on the outgoing edge follow the right part.
    const QString leftId = c.id;
    for (auto &tr : track.transitions)
        if (tr.clipA == leftId)
            tr.clipA = right.id;
    track.clips.insert(track.clips.begin() + idx + 1, right);
    return right.id;
}

void clearRange(Track &track, Ticks start, Ticks end)
{
    if (end <= start)
        return;
    splitAt(track, start);
    splitAt(track, end);
    std::set<QString> removed;
    track.clips.erase(std::remove_if(track.clips.begin(), track.clips.end(), [&](const Clip &c) {
        bool r = c.start >= start && c.end() <= end;
        if (r)
            removed.insert(c.id);
        return r;
    }), track.clips.end());
    track.transitions.erase(std::remove_if(track.transitions.begin(), track.transitions.end(), [&](const Transition &t) {
        return removed.count(t.clipA) || removed.count(t.clipB);
    }), track.transitions.end());
}

void shiftFrom(Track &track, Ticks from, Ticks delta)
{
    for (auto &c : track.clips)
        if (c.start >= from)
            c.start += delta;
    track.sortClips();
}

bool isRangeEmpty(const Track &track, Ticks start, Ticks end, const std::set<QString> &ignore)
{
    for (const auto &c : track.clips)
        if (!ignore.count(c.id) && c.start < end && start < c.end())
            return false;
    return true;
}

void overwrite(Track &track, Clip clip)
{
    clearRange(track, clip.start, clip.end());
    clip.ensureIntrinsics(track.type);
    track.clips.push_back(std::move(clip));
    track.sortClips();
}

// ---------------------------------------------------------------------------
bool overwriteEdit(Sequence &seq, const std::vector<std::pair<TrackRef, Clip>> &clips)
{
    for (const auto &[ref, c] : clips) {
        Track *t = seq.track(ref.type, ref.index);
        if (!t || t->locked || c.duration <= 0 || c.start < 0)
            return false;
    }
    for (const auto &[ref, c] : clips)
        overwrite(*seq.track(ref.type, ref.index), c);
    normalize(seq);
    return true;
}

bool insertEdit(Sequence &seq, const std::vector<std::pair<TrackRef, Clip>> &clips, Ticks at, Ticks duration)
{
    if (duration <= 0 || at < 0)
        return false;
    std::set<TrackRef> receiving;
    for (const auto &[ref, c] : clips) {
        Track *t = seq.track(ref.type, ref.index);
        if (!t || t->locked)
            return false;
        receiving.insert(ref);
    }
    for (const auto &ref : allTracks(seq)) {
        Track &t = *seq.track(ref.type, ref.index);
        if (receiving.count(ref) || t.syncLock) {
            splitAt(t, at);
            shiftFrom(t, at, duration);
        }
    }
    for (const auto &[ref, c] : clips)
        overwrite(*seq.track(ref.type, ref.index), c);
    // Markers after the insert point move too (Premiere behaviour).
    for (auto &m : seq.markers)
        if (m.time >= at)
            m.time += duration;
    normalize(seq);
    return true;
}

int razor(Sequence &seq, Ticks t, const std::vector<TrackRef> &tracks)
{
    std::vector<TrackRef> refs = tracks.empty() ? allTracks(seq) : tracks;
    std::map<QString, QString> newLinks;   // old link id -> link id of the right halves
    int count = 0;
    for (const auto &ref : refs) {
        Track *tr = seq.track(ref.type, ref.index);
        if (!tr || tr->locked)
            continue;
        QString rightId = splitAt(*tr, t);
        if (rightId.isEmpty())
            continue;
        ++count;
        Clip *r = tr->clip(rightId);
        if (r && !r->linkId.isEmpty()) {
            auto it = newLinks.find(r->linkId);
            if (it == newLinks.end())
                it = newLinks.emplace(r->linkId, newId()).first;
            r->linkId = it->second;
        }
    }
    normalize(seq);
    return count;
}

int razorClips(Sequence &seq, const std::vector<QString> &clipIds, Ticks t)
{
    std::set<TrackRef> refs;
    for (const auto &id : clipIds)
        for (const auto &lid : seq.linkedClips(id))
            if (auto r = seq.findClip(lid)) {
                const Clip &c = seq.tracks(r->type)[r->track].clips[r->index];
                if (t > c.start && t < c.end())
                    refs.insert({r->type, r->track});
            }
    return razor(seq, t, std::vector<TrackRef>(refs.begin(), refs.end()));
}

void lift(Sequence &seq, TimeRange range, const std::vector<TrackRef> &tracks)
{
    for (const auto &ref : tracks)
        if (Track *t = seq.track(ref.type, ref.index); t && !t->locked)
            clearRange(*t, range.start, range.end);
    normalize(seq);
}

void extract(Sequence &seq, TimeRange range, const std::vector<TrackRef> &tracks)
{
    if (range.empty())
        return;
    std::set<TrackRef> targets(tracks.begin(), tracks.end());
    for (const auto &ref : allTracks(seq)) {
        Track &t = *seq.track(ref.type, ref.index);
        if (!targets.count(ref) && !t.syncLock)
            continue;
        clearRange(t, range.start, range.end);
        shiftFrom(t, range.end, -range.duration());
    }
    for (auto &m : seq.markers)
        if (m.time >= range.end)
            m.time -= range.duration();
    normalize(seq);
}

void deleteClips(Sequence &seq, const std::vector<QString> &ids)
{
    std::set<QString> del(ids.begin(), ids.end());
    for (auto *v : {&seq.videoTracks, &seq.audioTracks})
        for (auto &t : *v) {
            if (t.locked)
                continue;
            t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(), [&](const Clip &c) { return del.count(c.id) > 0; }), t.clips.end());
        }
    normalize(seq);
}

void rippleDeleteClips(Sequence &seq, const std::vector<QString> &ids)
{
    // Collect ranges per track, process from the latest to the earliest.
    struct Item { TrackRef ref; Ticks start, end; QString id; };
    std::vector<Item> items;
    for (const auto &id : ids)
        if (auto r = seq.findClip(id)) {
            const Clip &c = seq.tracks(r->type)[r->track].clips[r->index];
            if (!seq.tracks(r->type)[r->track].locked)
                items.push_back({{r->type, r->track}, c.start, c.end(), id});
        }
    std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.start > b.start; });
    // Group overlapping ranges so linked V/A clips ripple once.
    std::vector<TimeRange> ranges;
    for (const auto &it : items) {
        bool merged = false;
        for (auto &r : ranges)
            if (it.start < r.end && r.start < it.end) {
                r.start = std::min(r.start, it.start);
                r.end = std::max(r.end, it.end);
                merged = true;
            }
        if (!merged)
            ranges.push_back({it.start, it.end});
    }
    std::sort(ranges.begin(), ranges.end(), [](const TimeRange &a, const TimeRange &b) { return a.start > b.start; });
    std::set<QString> del;
    for (const auto &it : items)
        del.insert(it.id);
    std::set<TrackRef> owning;
    for (const auto &it : items)
        owning.insert(it.ref);
    for (auto *v : {&seq.videoTracks, &seq.audioTracks})
        for (auto &t : *v)
            t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(), [&](const Clip &c) { return del.count(c.id) > 0; }), t.clips.end());
    for (const auto &r : ranges) {
        for (const auto &ref : allTracks(seq)) {
            Track &t = *seq.track(ref.type, ref.index);
            if (!owning.count(ref) && !t.syncLock)
                continue;
            if (!owning.count(ref) && !isRangeEmpty(t, r.start, r.end))
                continue;   // cannot ripple this track without destroying content
            if (owning.count(ref) && !isRangeEmpty(t, r.start, r.end))
                continue;   // other clips remain in the range on this track
            shiftFrom(t, r.end, -r.duration());
        }
    }
    normalize(seq);
}

static bool gapAt(const Track &t, Ticks at, TimeRange &gap)
{
    if (t.clipIndexAt(at) >= 0)
        return false;
    Ticks s = 0, e = INT64_MAX;
    for (const auto &c : t.clips) {
        if (c.end() <= at) s = std::max(s, c.end());
        if (c.start > at) e = std::min(e, c.start);
    }
    if (e == INT64_MAX)
        return false;   // no clip after: not a gap
    gap = {s, e};
    return true;
}

bool closeGap(Sequence &seq, TrackRef ref, Ticks at)
{
    Track *t = seq.track(ref.type, ref.index);
    TimeRange gap;
    if (!t || t->locked || !gapAt(*t, at, gap))
        return false;
    for (const auto &r : allTracks(seq)) {
        Track &o = *seq.track(r.type, r.index);
        if (r == ref || (o.syncLock && isRangeEmpty(o, gap.start, gap.end)))
            shiftFrom(o, gap.end, -gap.duration());
    }
    normalize(seq);
    return true;
}

void closeAllGaps(Sequence &seq)
{
    for (const auto &ref : allTracks(seq)) {
        Track &t = *seq.track(ref.type, ref.index);
        Ticks pos = 0;
        for (auto &c : t.clips) {
            if (c.start > pos)
                c.start = pos;
            pos = c.end();
        }
    }
    normalize(seq);
}

// ---------------------------------------------------------------------------
static bool sourceRangeOk(const Clip &c, const SourceLength &len)
{
    if (c.source != ClipSource::Media && c.source != ClipSource::Sequence)
        return true;
    Ticks L = len ? len(c) : unlimited(c);
    return c.in >= 0 && c.sourceOut() <= L + 2;   // tolerance for rounding
}

static Ticks prevEnd(const Track &t, const Clip &c)
{
    Ticks e = 0;
    for (const auto &o : t.clips)
        if (o.id != c.id && o.end() <= c.start)
            e = std::max(e, o.end());
    return e;
}
static Ticks nextStart(const Track &t, const Clip &c)
{
    Ticks s = INT64_MAX;
    for (const auto &o : t.clips)
        if (o.id != c.id && o.start >= c.end())
            s = std::min(s, o.start);
    return s;
}

static void shiftKeysOf(Clip &c, Ticks delta)
{
    for (auto &e : c.effects)
        for (auto &[k, p] : e.params)
            p.shiftKeys(delta);
}

bool trimIn(Sequence &seq, const QString &clipId, Ticks newStart, TrimMode mode, const SourceLength &len)
{
    auto r = seq.findClip(clipId);
    if (!r)
        return false;
    Track &t = seq.tracks(r->type)[r->track];
    if (t.locked)
        return false;
    Clip c = t.clips[r->index];
    const Ticks delta = newStart - c.start;
    if (delta == 0)
        return true;
    if (c.duration - delta < kMinDur)
        return false;
    Clip n = c;
    n.duration = c.duration - delta;
    if (!c.reverse)
        n.in = c.in + (Ticks)std::llround(double(delta) * c.speed);
    if (!sourceRangeOk(n, len))
        return false;
    shiftKeysOf(n, -delta);
    if (mode == TrimMode::Normal) {
        n.start = newStart;
        if (newStart < 0 || newStart < prevEnd(t, c))
            return false;
        t.clips[r->index] = n;
    } else {
        // Ripple: the clip keeps its start; everything after moves by -delta.
        n.start = c.start;
        const Ticks oldEnd = c.end();
        t.clips[r->index] = n;
        for (const auto &ref : allTracks(seq)) {
            Track &o = *seq.track(ref.type, ref.index);
            if (&o == &t || o.syncLock)
                shiftFrom(o, oldEnd, -delta);
        }
    }
    t.sortClips();
    normalize(seq);
    return true;
}

bool trimOut(Sequence &seq, const QString &clipId, Ticks newEnd, TrimMode mode, const SourceLength &len)
{
    auto r = seq.findClip(clipId);
    if (!r)
        return false;
    Track &t = seq.tracks(r->type)[r->track];
    if (t.locked)
        return false;
    Clip c = t.clips[r->index];
    const Ticks delta = newEnd - c.end();
    if (delta == 0)
        return true;
    if (c.duration + delta < kMinDur)
        return false;
    Clip n = c;
    n.duration = c.duration + delta;
    if (c.reverse)
        n.in = c.in - (Ticks)std::llround(double(delta) * c.speed);
    if (!sourceRangeOk(n, len))
        return false;
    if (mode == TrimMode::Normal) {
        if (newEnd > nextStart(t, c))
            return false;
        t.clips[r->index] = n;
    } else {
        const Ticks oldEnd = c.end();
        // Ripple: first move everything after, then extend.
        for (const auto &ref : allTracks(seq)) {
            Track &o = *seq.track(ref.type, ref.index);
            if (&o == &t || o.syncLock)
                shiftFrom(o, oldEnd, delta);
        }
        auto r2 = seq.findClip(clipId);
        seq.tracks(r2->type)[r2->track].clips[r2->index] = n;
    }
    normalize(seq);
    return true;
}

bool trimInLinked(Sequence &seq, const QString &clipId, Ticks newStart, TrimMode mode, const SourceLength &len)
{
    Sequence backup = seq;
    const Clip *c = seq.clip(clipId);
    if (!c)
        return false;
    const Ticks oldStart = c->start;
    for (const auto &id : seq.linkedClips(clipId)) {
        const Clip *o = seq.clip(id);
        if (!o || o->start != oldStart)
            continue;   // only linked clips sharing the edit point
        if (mode == TrimMode::Ripple && id != clipId) {
            // The ripple shift is applied once by the primary clip; others trim normally afterwards.
            continue;
        }
        if (!trimIn(seq, id, newStart, mode, len)) {
            seq = backup;
            return false;
        }
    }
    if (mode == TrimMode::Ripple) {
        const Ticks delta = newStart - oldStart;
        for (const auto &id : backup.linkedClips(clipId)) {
            if (id == clipId)
                continue;
            auto r = seq.findClip(id);
            if (!r)
                continue;
            Clip &o = seq.tracks(r->type)[r->track].clips[r->index];
            if (o.start != oldStart)
                continue;
            if (o.duration - delta < kMinDur) { seq = backup; return false; }
            o.duration -= delta;
            if (!o.reverse)
                o.in += (Ticks)std::llround(double(delta) * o.speed);
            shiftKeysOf(o, -delta);
            if (!sourceRangeOk(o, len)) { seq = backup; return false; }
        }
    }
    return true;
}

bool trimOutLinked(Sequence &seq, const QString &clipId, Ticks newEnd, TrimMode mode, const SourceLength &len)
{
    Sequence backup = seq;
    const Clip *c = seq.clip(clipId);
    if (!c)
        return false;
    const Ticks oldEnd = c->end();
    const Ticks delta = newEnd - oldEnd;
    std::vector<QString> ids = seq.linkedClips(clipId);
    if (mode == TrimMode::Normal) {
        for (const auto &id : ids) {
            const Clip *o = seq.clip(id);
            if (!o || o->end() != oldEnd)
                continue;
            if (!trimOut(seq, id, newEnd, mode, len)) { seq = backup; return false; }
        }
        return true;
    }
    if (!trimOut(seq, clipId, newEnd, TrimMode::Ripple, len)) { seq = backup; return false; }
    for (const auto &id : ids) {
        if (id == clipId)
            continue;
        auto r = seq.findClip(id);
        if (!r)
            continue;
        Clip &o = seq.tracks(r->type)[r->track].clips[r->index];
        // Linked clip's end was not shifted (its start is before oldEnd): extend it.
        if (o.end() != oldEnd)
            continue;
        if (o.duration + delta < kMinDur) { seq = backup; return false; }
        o.duration += delta;
        if (o.reverse)
            o.in -= (Ticks)std::llround(double(delta) * o.speed);
        if (!sourceRangeOk(o, len)) { seq = backup; return false; }
    }
    normalize(seq);
    return true;
}

bool roll(Sequence &seq, const QString &leftId, const QString &rightId, Ticks newCut, const SourceLength &len)
{
    Sequence backup = seq;
    Clip *a = seq.clip(leftId);
    Clip *b = seq.clip(rightId);
    if (!a || !b || a->end() != b->start)
        return false;
    const Ticks delta = newCut - a->end();
    Clip na = *a, nb = *b;
    na.duration += delta;
    if (na.reverse) na.in -= (Ticks)std::llround(double(delta) * na.speed);
    nb.duration -= delta;
    nb.start += delta;
    if (!nb.reverse) nb.in += (Ticks)std::llround(double(delta) * nb.speed);
    shiftKeysOf(nb, -delta);
    if (na.duration < kMinDur || nb.duration < kMinDur || !sourceRangeOk(na, len) || !sourceRangeOk(nb, len))
        return false;
    *a = na;
    *b = nb;
    normalize(seq);
    return true;
}

bool slip(Sequence &seq, const QString &clipId, Ticks sourceDelta, const SourceLength &len)
{
    Clip *c = seq.clip(clipId);
    if (!c)
        return false;
    Clip n = *c;
    n.in += sourceDelta;
    if (!sourceRangeOk(n, len))
        return false;
    *c = n;
    return true;
}

bool slide(Sequence &seq, const QString &clipId, Ticks delta, const SourceLength &len)
{
    auto r = seq.findClip(clipId);
    if (!r || delta == 0)
        return r.has_value();
    Track &t = seq.tracks(r->type)[r->track];
    if (t.locked)
        return false;
    Track backup = t;
    Clip &c = t.clips[r->index];
    Clip *prev = r->index > 0 && t.clips[r->index - 1].end() == c.start ? &t.clips[r->index - 1] : nullptr;
    Clip *next = r->index + 1 < (int)t.clips.size() && t.clips[r->index + 1].start == c.end() ? &t.clips[r->index + 1] : nullptr;
    const Ticks newStart = c.start + delta;
    if (newStart < 0)
        return false;
    if (prev) {
        prev->duration += delta;
        if (prev->reverse) prev->in -= (Ticks)std::llround(double(delta) * prev->speed);
        if (prev->duration < kMinDur || !sourceRangeOk(*prev, len)) { t = backup; return false; }
    } else if (newStart < prevEnd(t, c)) {
        t = backup;
        return false;
    }
    if (next) {
        next->start += delta;
        next->duration -= delta;
        if (!next->reverse) next->in += (Ticks)std::llround(double(delta) * next->speed);
        shiftKeysOf(*next, -delta);
        if (next->duration < kMinDur || !sourceRangeOk(*next, len)) { t = backup; return false; }
    } else if (c.end() + delta > nextStart(t, c)) {
        t = backup;
        return false;
    }
    c.start = newStart;
    normalize(seq);
    return true;
}

// ---------------------------------------------------------------------------
static std::vector<QString> expandLinks(const Sequence &seq, const std::vector<QString> &ids)
{
    std::set<QString> s;
    for (const auto &id : ids)
        s.insert(id);
    return {s.begin(), s.end()};
}

bool moveClips(Sequence &seq, const std::vector<QString> &idsIn, Ticks deltaT, int deltaVideo, int deltaAudio, bool insertMode)
{
    std::vector<QString> ids = expandLinks(seq, idsIn);
    struct Moving { Clip clip; TrackRef from, to; };
    std::vector<Moving> moving;
    for (const auto &id : ids) {
        auto r = seq.findClip(id);
        if (!r)
            continue;
        const Track &src = seq.tracks(r->type)[r->track];
        if (src.locked)
            return false;
        Moving m{src.clips[r->index], {r->type, r->track}, {r->type, r->track + (r->type == TrackType::Video ? deltaVideo : deltaAudio)}};
        m.clip.start += deltaT;
        if (m.clip.start < 0)
            return false;
        Track *dst = seq.track(m.to.type, m.to.index);
        if (!dst || dst->locked)
            return false;
        moving.push_back(m);
    }
    if (moving.empty())
        return false;
    // Transitions travel with their clips when both ends move on the same track.
    std::set<QString> movingIds;
    for (const auto &m : moving)
        movingIds.insert(m.clip.id);
    std::map<TrackRef, std::vector<Transition>> carried;
    for (const auto &m : moving) {
        Track &src = *seq.track(m.from.type, m.from.index);
        for (const auto &tr : src.transitions) {
            bool aOk = tr.clipA.isEmpty() || movingIds.count(tr.clipA);
            bool bOk = tr.clipB.isEmpty() || movingIds.count(tr.clipB);
            bool touches = tr.clipA == m.clip.id || tr.clipB == m.clip.id;
            if (touches && aOk && bOk) {
                auto &vec = carried[m.to];
                if (std::none_of(vec.begin(), vec.end(), [&](const Transition &x) { return x.id == tr.id; }))
                    vec.push_back(tr);
            }
        }
    }
    // Remove from sources.
    for (const auto &m : moving) {
        Track &src = *seq.track(m.from.type, m.from.index);
        src.clips.erase(std::remove_if(src.clips.begin(), src.clips.end(), [&](const Clip &c) { return c.id == m.clip.id; }), src.clips.end());
    }
    if (insertMode) {
        // Ripple insert: open space at the earliest destination start.
        Ticks at = INT64_MAX, endMax = 0;
        for (const auto &m : moving) {
            at = std::min(at, m.clip.start);
            endMax = std::max(endMax, m.clip.end());
        }
        std::vector<std::pair<TrackRef, Clip>> list;
        for (const auto &m : moving)
            list.push_back({m.to, m.clip});
        if (!insertEdit(seq, list, at, endMax - at))
            return false;
    } else {
        for (const auto &m : moving)
            overwrite(*seq.track(m.to.type, m.to.index), m.clip);
    }
    for (auto &[ref, trs] : carried) {
        Track &dst = *seq.track(ref.type, ref.index);
        for (const auto &tr : trs)
            if (!dst.transition(tr.id))
                dst.transitions.push_back(tr);
    }
    normalize(seq);
    return true;
}

std::vector<QString> duplicateClips(Sequence &seq, const std::vector<QString> &ids, Ticks deltaT, int deltaVideo, int deltaAudio)
{
    std::vector<QString> out;
    std::map<QString, QString> linkMap, groupMap;
    std::vector<std::pair<TrackRef, Clip>> list;
    for (const auto &id : ids) {
        auto r = seq.findClip(id);
        if (!r)
            continue;
        Clip c = seq.tracks(r->type)[r->track].clips[r->index];
        c.id = newId();
        regenerateUids(c);
        c.start += deltaT;
        if (!c.linkId.isEmpty()) {
            if (!linkMap.count(c.linkId)) linkMap[c.linkId] = newId();
            c.linkId = linkMap[c.linkId];
        }
        if (!c.groupId.isEmpty()) {
            if (!groupMap.count(c.groupId)) groupMap[c.groupId] = newId();
            c.groupId = groupMap[c.groupId];
        }
        TrackRef to{r->type, r->track + (r->type == TrackType::Video ? deltaVideo : deltaAudio)};
        if (!seq.track(to.type, to.index) || c.start < 0)
            return {};
        list.push_back({to, c});
        out.push_back(c.id);
    }
    if (!overwriteEdit(seq, list))
        return {};
    return out;
}

bool setSpeed(Sequence &seq, const QString &clipId, double speed, bool reverse, bool ripple, const SourceLength &len)
{
    if (speed <= 0.0001)
        return false;
    auto r = seq.findClip(clipId);
    if (!r)
        return false;
    Track &t = seq.tracks(r->type)[r->track];
    Clip c = t.clips[r->index];
    const Ticks srcDur = c.sourceDuration();
    Ticks newDur = std::max<Ticks>(kMinDur, (Ticks)std::llround(double(srcDur) / speed));
    const Ticks oldEnd = c.end();
    const double ratio = c.speed / speed;
    Clip n = c;
    n.speed = speed;
    n.reverse = reverse;
    if (!ripple) {
        Ticks limit = nextStart(t, c) - c.start;
        newDur = std::min(newDur, limit);
    }
    n.duration = newDur;
    for (auto &e : n.effects)
        for (auto &[k, p] : e.params)
            for (auto &kf : p.keys())
                kf.time = (Ticks)std::llround(double(kf.time) * ratio);
    if (!sourceRangeOk(n, len))
        return false;
    t.clips[r->index] = n;
    if (ripple) {
        const Ticks delta = n.end() - oldEnd;
        for (const auto &ref : allTracks(seq)) {
            Track &o = *seq.track(ref.type, ref.index);
            if (&o == &t || o.syncLock)
                shiftFrom(o, oldEnd, delta);
        }
    }
    normalize(seq);
    return true;
}

bool rateStretch(Sequence &seq, const QString &clipId, Ticks newDuration, bool fromHead)
{
    auto r = seq.findClip(clipId);
    if (!r || newDuration < kMinDur)
        return false;
    Track &t = seq.tracks(r->type)[r->track];
    Clip &c = t.clips[r->index];
    const Ticks srcDur = c.sourceDuration();
    Clip n = c;
    n.speed = double(srcDur) / double(newDuration);
    n.duration = newDuration;
    if (fromHead)
        n.start = c.end() - newDuration;
    if (n.start < prevEnd(t, c) || n.end() > nextStart(t, c))
        return false;
    c = n;
    normalize(seq);
    return true;
}

void link(Sequence &seq, const std::vector<QString> &ids)
{
    const QString l = newId();
    for (const auto &id : ids)
        if (Clip *c = seq.clip(id))
            c->linkId = l;
}
void unlink(Sequence &seq, const std::vector<QString> &ids)
{
    for (const auto &id : ids)
        if (Clip *c = seq.clip(id))
            c->linkId.clear();
}
void group(Sequence &seq, const std::vector<QString> &ids)
{
    const QString g = newId();
    for (const auto &id : ids)
        if (Clip *c = seq.clip(id))
            c->groupId = g;
}
void ungroup(Sequence &seq, const std::vector<QString> &ids)
{
    for (const auto &id : ids)
        if (Clip *c = seq.clip(id))
            c->groupId.clear();
}

// ---------------------------------------------------------------------------
TimeRange transitionRange(const Track &track, const Transition &t)
{
    const Clip *a = t.clipA.isEmpty() ? nullptr : track.clip(t.clipA);
    const Clip *b = t.clipB.isEmpty() ? nullptr : track.clip(t.clipB);
    if (a && b) {
        const Ticks cut = a->end();
        switch (t.alignment) {
        case Transition::StartAtCut: return {cut, cut + t.duration};
        case Transition::EndAtCut: return {cut - t.duration, cut};
        case Transition::CenterAtCut:
        default: return {cut - t.duration / 2, cut - t.duration / 2 + t.duration};
        }
    }
    if (b)
        return {b->start, b->start + t.duration};
    if (a)
        return {a->end() - t.duration, a->end()};
    return {0, 0};
}

QString addTransition(Sequence &seq, TrackRef ref, const QString &clipA, const QString &clipB, const QString &type, Ticks duration,
                      Transition::Alignment align)
{
    Track *t = seq.track(ref.type, ref.index);
    if (!t || t->locked || duration <= 0)
        return {};
    // Replace an existing transition at the same place.
    t->transitions.erase(std::remove_if(t->transitions.begin(), t->transitions.end(), [&](const Transition &x) {
        return (!clipA.isEmpty() && x.clipA == clipA) || (!clipB.isEmpty() && x.clipB == clipB);
    }), t->transitions.end());
    Transition tr;
    tr.id = newId();
    tr.clipA = clipA;
    tr.clipB = clipB;
    tr.duration = duration;
    tr.alignment = (clipA.isEmpty() || clipB.isEmpty()) ? Transition::CenterAtCut : align;
    tr.effect = EffectInstance::create(type);
    t->transitions.push_back(tr);
    normalize(seq);
    return t->transition(tr.id) ? tr.id : QString();
}

void normalize(Sequence &seq)
{
    for (auto *v : {&seq.videoTracks, &seq.audioTracks})
        for (auto &t : *v) {
            t.sortClips();
            t.transitions.erase(std::remove_if(t.transitions.begin(), t.transitions.end(), [&](Transition &tr) {
                const Clip *a = tr.clipA.isEmpty() ? nullptr : t.clip(tr.clipA);
                const Clip *b = tr.clipB.isEmpty() ? nullptr : t.clip(tr.clipB);
                if (!tr.clipA.isEmpty() && !a) return true;
                if (!tr.clipB.isEmpty() && !b) return true;
                if (!a && !b) return true;
                if (a && b && a->end() != b->start) return true;
                // Clamp duration to the clips it covers.
                Ticks maxDur = INT64_MAX;
                if (a && b) {
                    if (tr.alignment == Transition::CenterAtCut) maxDur = 2 * std::min(a->duration, b->duration);
                    else if (tr.alignment == Transition::StartAtCut) maxDur = b->duration;
                    else maxDur = a->duration;
                } else if (a) maxDur = a->duration;
                else maxDur = b->duration;
                tr.duration = std::min(tr.duration, maxDur);
                return tr.duration <= 0;
            }), t.transitions.end());
        }
}

Ticks nextEdit(const Sequence &seq, Ticks t, bool forward, const std::vector<TrackRef> &tracks)
{
    std::vector<TrackRef> refs = tracks.empty() ? allTracks(seq, true) : tracks;
    Ticks best = forward ? INT64_MAX : INT64_MIN;
    auto consider = [&](Ticks p) {
        if (forward && p > t && p < best) best = p;
        if (!forward && p < t && p > best) best = p;
    };
    for (const auto &ref : refs)
        if (const Track *tr = seq.track(ref.type, ref.index))
            for (const auto &c : tr->clips) {
                consider(c.start);
                consider(c.end());
            }
    consider(0);
    if (best == INT64_MAX || best == INT64_MIN)
        return t;
    return best;
}

std::vector<Ticks> snapPoints(const Sequence &seq, const std::set<QString> &exclude)
{
    std::vector<Ticks> pts{0, seq.playhead};
    for (const auto *v : {&seq.videoTracks, &seq.audioTracks})
        for (const auto &t : *v)
            for (const auto &c : t.clips)
                if (!exclude.count(c.id)) {
                    pts.push_back(c.start);
                    pts.push_back(c.end());
                }
    for (const auto &m : seq.markers)
        pts.push_back(m.time);
    if (seq.inPoint != kInvalidTicks) pts.push_back(seq.inPoint);
    if (seq.outPoint != kInvalidTicks) pts.push_back(seq.outPoint);
    std::sort(pts.begin(), pts.end());
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    return pts;
}

QString nest(Project &project, Sequence &seq, const std::vector<QString> &ids, const QString &name)
{
    if (ids.empty())
        return {};
    Ticks first = INT64_MAX, last = 0;
    int minV = INT_MAX, minA = INT_MAX;
    struct Item { Sequence::ClipRef ref; Clip clip; };
    std::vector<Item> items;
    for (const auto &id : ids)
        if (auto r = seq.findClip(id)) {
            const Clip &c = seq.tracks(r->type)[r->track].clips[r->index];
            items.push_back({*r, c});
            first = std::min(first, c.start);
            last = std::max(last, c.end());
            if (r->type == TrackType::Video) minV = std::min(minV, r->track);
            else minA = std::min(minA, r->track);
        }
    if (items.empty())
        return {};
    Sequence nested;
    nested.id = newId();
    nested.name = name;
    nested.width = seq.width;
    nested.height = seq.height;
    nested.fps = seq.fps;
    nested.sampleRate = seq.sampleRate;
    nested.pixelAspect = seq.pixelAspect;
    nested.binId = seq.binId;
    for (const auto &it : items) {
        auto &tracks = nested.tracks(it.ref.type);
        while ((int)tracks.size() <= it.ref.track - (it.ref.type == TrackType::Video ? minV : minA))
            nested.addTrack(it.ref.type);
        Clip c = it.clip;
        c.start -= first;
        c.linkId.clear();
        tracks[it.ref.track - (it.ref.type == TrackType::Video ? minV : minA)].clips.push_back(c);
    }
    // Carry transitions fully inside the selection.
    std::set<QString> sel(ids.begin(), ids.end());
    for (int k = 0; k < 2; ++k) {
        TrackType tt = k ? TrackType::Audio : TrackType::Video;
        int minT = tt == TrackType::Video ? minV : minA;
        for (int ti = 0; ti < (int)seq.tracks(tt).size(); ++ti)
            for (const auto &tr : seq.tracks(tt)[ti].transitions)
                if ((tr.clipA.isEmpty() || sel.count(tr.clipA)) && (tr.clipB.isEmpty() || sel.count(tr.clipB)) &&
                    (sel.count(tr.clipA) || sel.count(tr.clipB)) && ti - minT >= 0 && ti - minT < (int)nested.tracks(tt).size())
                    nested.tracks(tt)[ti - minT].transitions.push_back(tr);
    }
    nested.ensureDefaultTracks();
    for (auto &t : nested.videoTracks) t.sortClips();
    for (auto &t : nested.audioTracks) t.sortClips();
    deleteClips(seq, ids);
    const QString link = newId();
    std::vector<std::pair<TrackRef, Clip>> place;
    if (minV != INT_MAX) {
        Clip v;
        v.id = newId();
        v.name = name;
        v.source = ClipSource::Sequence;
        v.mediaId = nested.id;
        v.start = first;
        v.duration = last - first;
        v.linkId = link;
        v.ensureIntrinsics(TrackType::Video);
        place.push_back({{TrackType::Video, minV}, v});
    }
    if (minA != INT_MAX) {
        Clip a;
        a.id = newId();
        a.name = name;
        a.source = ClipSource::Sequence;
        a.mediaId = nested.id;
        a.start = first;
        a.duration = last - first;
        a.linkId = link;
        a.ensureIntrinsics(TrackType::Audio);
        place.push_back({{TrackType::Audio, minA}, a});
    }
    const QString nestedId = nested.id;
    const QString seqId = seq.id;
    bool inProject = project.sequence(seqId) == &seq;
    if (!inProject)
        overwriteEdit(seq, place);
    project.sequences.push_back(std::move(nested));
    // `seq` may have been invalidated by push_back if it lives in project.sequences.
    if (inProject)
        overwriteEdit(*project.sequence(seqId), place);
    return nestedId;
}

} // namespace rc::edit
