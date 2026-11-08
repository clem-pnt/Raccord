#pragma once
// Pure timeline editing operations on a Sequence. They never touch the UI and
// are fully unit tested. Every function returns false (and leaves the sequence
// untouched) when the edit is not possible.
#include "Model.h"
#include <functional>
#include <set>

namespace rc::edit {

struct TrackRef {
    TrackType type;
    int index;
    bool operator<(const TrackRef &o) const { return type != o.type ? type < o.type : index < o.index; }
    bool operator==(const TrackRef &o) const { return type == o.type && index == o.index; }
};

// Returns the usable source length of a clip (INT64_MAX for stills/generators).
using SourceLength = std::function<Ticks(const Clip &)>;
Ticks unlimited(const Clip &);

enum class TrimMode { Normal, Ripple };

// ---- low level (single track) ----
// Splits the clip covering t (strictly inside). Returns the id of the right part or empty.
QString splitAt(Track &track, Ticks t);
// Removes all content in [start, end) (splitting clips at the edges).
void clearRange(Track &track, Ticks start, Ticks end);
// Moves every clip starting at or after `from` by delta.
void shiftFrom(Track &track, Ticks from, Ticks delta);
bool isRangeEmpty(const Track &track, Ticks start, Ticks end, const std::set<QString> &ignore = {});
// Places a clip, overwriting whatever is under it.
void overwrite(Track &track, Clip clip);

// ---- sequence level ----
// Overwrite edit of several clips (e.g. linked V+A) at their own start positions.
bool overwriteEdit(Sequence &seq, const std::vector<std::pair<TrackRef, Clip>> &clips);
// Insert edit: everything at or after `at` on sync-locked tracks moves right by `duration`.
bool insertEdit(Sequence &seq, const std::vector<std::pair<TrackRef, Clip>> &clips, Ticks at, Ticks duration);
// Razor (add edit) on the given tracks; empty = all unlocked tracks.
int razor(Sequence &seq, Ticks t, const std::vector<TrackRef> &tracks = {});
// Razor only the given clips (and their links) at t.
int razorClips(Sequence &seq, const std::vector<QString> &clipIds, Ticks t);
// Lift: removes content in range on the tracks, leaving a gap.
void lift(Sequence &seq, TimeRange range, const std::vector<TrackRef> &tracks);
// Extract: removes the range and closes the gap on the tracks and on every sync-locked track.
void extract(Sequence &seq, TimeRange range, const std::vector<TrackRef> &tracks);
void deleteClips(Sequence &seq, const std::vector<QString> &ids);
void rippleDeleteClips(Sequence &seq, const std::vector<QString> &ids);
// Deletes the gap at t on the track (and shifts sync-locked tracks if they are empty there too).
bool closeGap(Sequence &seq, TrackRef track, Ticks t);
// Removes every gap on the given tracks (or all video/audio tracks).
void closeAllGaps(Sequence &seq);

bool trimIn(Sequence &seq, const QString &clipId, Ticks newStart, TrimMode mode, const SourceLength &len);
bool trimOut(Sequence &seq, const QString &clipId, Ticks newEnd, TrimMode mode, const SourceLength &len);
// Trims linked clips together.
bool trimInLinked(Sequence &seq, const QString &clipId, Ticks newStart, TrimMode mode, const SourceLength &len);
bool trimOutLinked(Sequence &seq, const QString &clipId, Ticks newEnd, TrimMode mode, const SourceLength &len);
bool roll(Sequence &seq, const QString &leftClipId, const QString &rightClipId, Ticks newCut, const SourceLength &len);
bool slip(Sequence &seq, const QString &clipId, Ticks sourceDelta, const SourceLength &len);
bool slide(Sequence &seq, const QString &clipId, Ticks delta, const SourceLength &len);
// Moves clips by deltaT and track offsets. insertMode = ripple insert at the destination.
bool moveClips(Sequence &seq, const std::vector<QString> &ids, Ticks deltaT, int deltaVideo, int deltaAudio, bool insertMode = false);
// Copies clips (Alt drag / paste) with new ids, overwriting at destination.
std::vector<QString> duplicateClips(Sequence &seq, const std::vector<QString> &ids, Ticks deltaT, int deltaVideo, int deltaAudio);
bool setSpeed(Sequence &seq, const QString &clipId, double speed, bool reverse, bool ripple, const SourceLength &len);
// Rate stretch tool: change duration by changing speed.
bool rateStretch(Sequence &seq, const QString &clipId, Ticks newDuration, bool fromHead);

void link(Sequence &seq, const std::vector<QString> &ids);
void unlink(Sequence &seq, const std::vector<QString> &ids);
void group(Sequence &seq, const std::vector<QString> &ids);
void ungroup(Sequence &seq, const std::vector<QString> &ids);

// Adds a transition at a cut (between two adjacent clips) or a clip edge.
QString addTransition(Sequence &seq, TrackRef track, const QString &clipA, const QString &clipB, const QString &type, Ticks duration,
                      Transition::Alignment align = Transition::CenterAtCut);
// Effective range of a transition on the timeline.
TimeRange transitionRange(const Track &track, const Transition &t);
// Removes invalid transitions and clamps them to clip lengths.
void normalize(Sequence &seq);

// Next/previous edit point across the given tracks (or all).
Ticks nextEdit(const Sequence &seq, Ticks t, bool forward, const std::vector<TrackRef> &tracks = {});
// Snap candidates: clip edges, markers, playhead, in/out.
std::vector<Ticks> snapPoints(const Sequence &seq, const std::set<QString> &excludeClips = {});

// Replaces clips by a single nested sequence clip. Returns the new sequence id.
QString nest(Project &project, Sequence &seq, const std::vector<QString> &ids, const QString &name);

} // namespace rc::edit
