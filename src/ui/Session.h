#pragma once
// Shared editing state for the UI: document, selection, playhead, tools.
#include "core/Document.h"
#include "core/Edit.h"
#include <QObject>
#include <QSet>
#include <memory>

namespace rc {

enum class Tool { Selection, TrackSelectForward, TrackSelectBackward, Ripple, Rolling, RateStretch, Razor, Slip, Slide, Pen, Hand, Zoom, Type };

class Session : public QObject {
    Q_OBJECT
public:
    explicit Session(QObject *parent = nullptr);
    static Session *instance();

    Document &doc() { return m_doc; }
    Project &project() { return m_doc.project(); }
    Sequence *sequence() { return m_doc.project().activeSequence(); }
    // Immutable copy of the project for background threads (playback audio, prefetch).
    std::shared_ptr<const Project> snapshot();

    // Selection (clip ids in the active sequence).
    const QSet<QString> &selection() const { return m_selection; }
    void select(const QSet<QString> &ids, bool expandLinks = true);
    void clearSelection() { select({}); }
    QString primaryClip() const { return m_primary; }
    QString selectedTransition() const { return m_transition; }
    void selectTransition(const QString &id);
    bool linkedSelection() const { return m_linkedSelection; }
    void setLinkedSelection(bool on) { m_linkedSelection = on; emit settingsChanged(); }
    bool snapping() const { return m_snap; }
    void setSnapping(bool on) { m_snap = on; emit settingsChanged(); }

    Ticks playhead() const;
    void setPlayhead(Ticks t, bool fromPlayback = false);
    Tool tool() const { return m_tool; }
    void setTool(Tool t) { m_tool = t; emit toolChanged(t); }

    // Source monitor content.
    QString sourceMedia() const { return m_sourceMedia; }
    void setSourceMedia(const QString &mediaId);
    Ticks sourcePosition() const { return m_sourcePos; }
    void setSourcePosition(Ticks t);

    void setActiveSequence(const QString &id);
    edit::SourceLength sourceLength() const;
    // Opens a "track targeting" aware list of destination tracks for source edits.
    std::vector<edit::TrackRef> targetedTracks() const;
    // The panel that currently drives the program/source split for shortcuts.
    enum class Focus { Timeline, Program, Source, Project, Other };
    Focus focus() const { return m_focus; }
    // Builds linked video/audio clips for a media item placed at `at`.
    // in/out are source times (kInvalidTicks = use the item's marks or full length).
    std::vector<std::pair<edit::TrackRef, Clip>> makeClips(const MediaItem &m, Ticks at, int videoTrack, int audioTrack,
                                                           bool video = true, bool audio = true);
    // Adjusts an empty sequence to the media's size/rate (first clip dropped).
    void matchSequenceToMedia(Sequence &seq, const MediaItem &m);
    // Synthetic clip for an Effects browser entry "title:<template>" or "gen:<kind>".
    std::optional<Clip> makeGenerator(const QString &id, Ticks at, const Sequence &seq);
    void setFocus(Focus f) { m_focus = f; }

signals:
    void selectionChanged();
    void playheadChanged(Ticks t);
    void toolChanged(Tool t);
    void sourceChanged();
    void sourcePositionChanged(Ticks t);
    void sequenceChanged();       // active sequence switched
    void settingsChanged();
    void statusMessage(const QString &msg);

private:
    Document m_doc;
    QSet<QString> m_selection;
    QString m_primary;
    QString m_transition;
    Tool m_tool = Tool::Selection;
    bool m_linkedSelection = true;
    bool m_snap = true;
    QString m_sourceMedia;
    Ticks m_sourcePos = 0;
    Focus m_focus = Focus::Timeline;
    std::shared_ptr<const Project> m_snapshot;
    bool m_snapshotDirty = true;
};

} // namespace rc
