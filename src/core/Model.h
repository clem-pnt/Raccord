#pragma once
// Project data model: media, bins, sequences, tracks, clips, transitions.
// Plain value types: copying a Sequence gives an independent snapshot, which
// the undo system relies on.
#include "Time.h"
#include "Effects.h"
#include <QColor>
#include <QJsonObject>
#include <QString>
#include <vector>
#include <optional>

namespace rc {

QString newId();

struct MediaInfo {
    bool valid = false;
    QString error;
    QString container;
    Ticks duration = 0;
    Ticks startTime = 0;           // first pts
    QString timecode;              // embedded start timecode if any
    int64_t bitrate = 0;
    bool hasVideo = false;
    bool hasAudio = false;
    bool isStill = false;          // single image
    bool isImageSequence = false;
    // video
    int width = 0, height = 0;
    double pixelAspect = 1.0;
    FrameRate fps{25, 1};
    bool variableFrameRate = false;
    QString videoCodec;
    QString pixelFormat;
    int bitDepth = 8;
    int rotation = 0;              // degrees from display matrix
    QString colorPrimaries, colorTransfer, colorSpace;
    bool fullRange = false;
    bool hdr = false;
    bool hasAlpha = false;
    bool interlaced = false;
    // audio
    int audioStreams = 0;
    int channels = 0;
    int sampleRate = 0;
    QString audioCodec;

    QJsonObject toJson() const;
    static MediaInfo fromJson(const QJsonObject &o);
    QString summary() const;
};

struct Marker {
    enum Type { Comment, Chapter, Segmentation, WebLink, FlashCue };
    QString id;
    Ticks time = 0;
    Ticks duration = 0;
    QString name;
    QString comment;
    int color = 0;   // index in labelColors()
    Type type = Comment;

    QJsonObject toJson() const;
    static Marker fromJson(const QJsonObject &o);
};

const std::vector<QColor> &labelColors();
QStringList labelColorNames();

struct MediaItem {
    QString id;
    QString name;
    QString path;
    QString binId;            // empty = root
    QString proxyPath;        // generated proxy file (may be empty)
    MediaInfo info;
    int label = 0;
    QString comment;
    QString scene, shot, take;   // log metadata
    bool favourite = false;
    Ticks inPoint = kInvalidTicks;
    Ticks outPoint = kInvalidTicks;
    std::vector<Marker> markers;
    // Interpret footage overrides
    std::optional<FrameRate> fpsOverride;
    int alphaMode = 0;         // 0 auto, 1 ignore, 2 premultiplied, 3 straight
    // Audio analysis results (filled lazily)
    QString waveformPath;
    double loudnessLufs = 0;   // 0 = not analysed

    bool isImage() const { return info.isStill; }
    FrameRate fps() const { return fpsOverride ? *fpsOverride : info.fps; }
    QJsonObject toJson() const;
    static MediaItem fromJson(const QJsonObject &o);
};

struct Bin {
    QString id;
    QString name;
    QString parentId;
    QJsonObject toJson() const;
    static Bin fromJson(const QJsonObject &o);
};

enum class TrackType { Video, Audio };

enum class ClipSource {
    Media,        // a MediaItem
    Sequence,     // nested sequence
    Title,        // text/graphics document in Clip::gen
    Solid,        // colour matte
    Adjustment,   // adjustment layer: applies its effects to the tracks below
    Bars,         // SMPTE bars and tone
    Countdown,    // universal counting leader
    Transparent,  // transparent video
};
QString clipSourceName(ClipSource s);
ClipSource clipSourceFromName(const QString &s);

struct Clip {
    QString id;
    QString name;
    ClipSource source = ClipSource::Media;
    QString mediaId;        // MediaItem id or Sequence id (nested)
    Ticks start = 0;        // position on the sequence
    Ticks duration = 0;     // length on the sequence
    Ticks in = 0;           // source in point (media time, ticks)
    double speed = 1.0;     // playback rate (>0); reverse handled by flag
    bool reverse = false;
    bool maintainPitch = true;
    bool enabled = true;
    int audioStream = 0;    // audio stream index for audio clips
    int channelMap = -1;    // -1 = all channels mixed per track layout, else 0-based channel
    QString linkId;         // clips sharing a link id move/select together (A/V link)
    QString groupId;
    int label = 0;
    std::vector<EffectInstance> effects;
    QJsonObject gen;        // generator payload (title document, solid colour...)

    Ticks end() const { return start + duration; }
    // Source media time for a sequence time inside the clip.
    Ticks sourceAt(Ticks seqTime) const;
    Ticks sourceDuration() const { return (Ticks)std::llround(double(duration) * speed); }
    Ticks sourceOut() const { return in + sourceDuration(); }
    // Local time relative to the clip start (used for keyframes).
    Ticks localTime(Ticks seqTime) const { return seqTime - start; }

    EffectInstance *effect(const QString &type);
    const EffectInstance *effect(const QString &type) const;
    EffectInstance *effectByUid(const QString &uid);
    // Ensures intrinsic effects (motion/opacity or volume/panner) exist.
    void ensureIntrinsics(TrackType t);

    QJsonObject toJson() const;
    static Clip fromJson(const QJsonObject &o);
};

// Multicam clip (nested multicam sequence): active angle = video track index of the source sequence.
inline int multicamAngle(const Clip &c) { return c.gen.value("angle").toInt(0); }
// Audio of a multicam clip follows the active angle (its audio track) instead of the source sequence mix.
inline bool multicamAudioFollows(const Clip &c) { return c.gen.value("audioFollowsVideo").toBool(false); }

struct Transition {
    enum Alignment { CenterAtCut, StartAtCut, EndAtCut };
    QString id;
    QString clipA;          // outgoing clip id (may be empty: transition at start of clipB)
    QString clipB;          // incoming clip id (may be empty: transition at end of clipA)
    Ticks duration = 0;
    Alignment alignment = CenterAtCut;
    EffectInstance effect;  // type = transition id

    QJsonObject toJson() const;
    static Transition fromJson(const QJsonObject &o);
};

struct Track {
    QString id;
    QString name;
    TrackType type = TrackType::Video;
    bool muted = false;       // audio mute / video "eye" off
    bool solo = false;
    bool locked = false;
    bool syncLock = true;
    bool targeted = true;
    bool sourcePatched = true;
    int height = 0;           // UI height (0 = default)
    int channels = 2;         // audio track layout: 1 mono, 2 stereo, 6 5.1
    std::vector<Clip> clips;  // sorted by start, non overlapping
    std::vector<Transition> transitions;
    std::vector<EffectInstance> effects;   // track effects (audio mixer inserts)
    double volumeDb = 0.0;
    double pan = 0.0;

    void sortClips();
    Clip *clip(const QString &id);
    const Clip *clip(const QString &id) const;
    int clipIndexAt(Ticks t) const;       // clip covering t, -1 if gap
    Ticks endTime() const;
    Transition *transition(const QString &id);

    QJsonObject toJson() const;
    static Track fromJson(const QJsonObject &o);
};

struct Caption {
    QString id;
    Ticks start = 0;
    Ticks end = 0;
    QString text;
    QString speaker;
    QJsonObject toJson() const;
    static Caption fromJson(const QJsonObject &o);
};

struct CaptionStyle {
    QString font = "Inter";
    double size = 0.045;        // fraction of frame height
    QColor color = Qt::white;
    QColor outline = Qt::black;
    double outlineWidth = 0.08; // fraction of font size
    QColor background = QColor(0, 0, 0, 0);
    double positionY = 0.9;     // baseline region centre
    bool bold = true;
    bool uppercase = false;
    int maxLines = 2;
    int maxCharsPerLine = 42;
    bool wordHighlight = false; // karaoke style (social captions)
    QColor highlight = QColor(255, 220, 0);
    QJsonObject toJson() const;
    static CaptionStyle fromJson(const QJsonObject &o);
};

struct CaptionTrack {
    QString id;
    QString name = "Subtitles";
    QString language = "fr";
    bool visible = true;
    bool burnIn = false;
    CaptionStyle style;
    std::vector<Caption> captions;
    void sort();
    QJsonObject toJson() const;
    static CaptionTrack fromJson(const QJsonObject &o);
};

struct Sequence {
    QString id;
    QString name = "Sequence 01";
    QString binId;
    int width = 1920;
    int height = 1080;
    double pixelAspect = 1.0;
    FrameRate fps{25, 1};
    int sampleRate = 48000;
    int audioChannels = 2;
    Ticks startTimecode = 0;  // displayed timecode offset (e.g. 01:00:00:00)
    std::vector<Track> videoTracks;  // index 0 = V1 (bottom)
    std::vector<Track> audioTracks;  // index 0 = A1 (top)
    std::vector<CaptionTrack> captionTracks;
    std::vector<Marker> markers;
    std::vector<EffectInstance> masterEffects;  // master audio bus inserts
    double masterVolumeDb = 0.0;
    Ticks inPoint = kInvalidTicks;
    Ticks outPoint = kInvalidTicks;
    Ticks playhead = 0;
    QString colorSpace = "rec709";   // working space: rec709, rec2020pq, rec2020hlg
    bool multicam = false;           // multi-camera source sequence: one angle per video track

    Ticks duration() const;
    Track *track(TrackType t, int index);
    const Track *track(TrackType t, int index) const;
    std::vector<Track> &tracks(TrackType t) { return t == TrackType::Video ? videoTracks : audioTracks; }
    const std::vector<Track> &tracks(TrackType t) const { return t == TrackType::Video ? videoTracks : audioTracks; }
    Track *addTrack(TrackType t);
    // Finds a clip anywhere in the sequence.
    struct ClipRef { TrackType type; int track; int index; };
    std::optional<ClipRef> findClip(const QString &clipId) const;
    Clip *clip(const QString &clipId);
    const Clip *clip(const QString &clipId) const;
    std::vector<QString> linkedClips(const QString &clipId) const;   // includes itself
    TimeRange workRange() const;    // in/out or whole sequence
    void ensureDefaultTracks();
    void renameTracks();

    QJsonObject toJson() const;
    static Sequence fromJson(const QJsonObject &o);
};

struct ProjectSettings {
    int proxyHeight = 540;
    QString proxyCodec = "h264";     // h264 (all-intra), prores, dnxhr
    bool useProxies = true;
    QString scratchDir;              // empty = next to the project in "<name> Cache"
    int autosaveMinutes = 5;
    int autosaveKeep = 20;
    Ticks defaultStillDuration = 5 * kTicksPerSecond;
    Ticks defaultVideoTransition = kTicksPerSecond;      // 1 s
    Ticks defaultAudioTransition = kTicksPerSecond / 2;
    QString defaultVideoTransitionType = "crossDissolve";
    QString defaultAudioTransitionType = "constantPower";
    int playbackResolution = 1;      // 1 full, 2 half, 4 quarter, 8
    QJsonObject toJson() const;
    static ProjectSettings fromJson(const QJsonObject &o);
};

struct Project {
    QString name = "Untitled";
    std::vector<MediaItem> media;
    std::vector<Bin> bins;
    std::vector<Sequence> sequences;
    QString activeSequenceId;
    ProjectSettings settings;

    MediaItem *mediaItem(const QString &id);
    const MediaItem *mediaItem(const QString &id) const;
    MediaItem *mediaByPath(const QString &path);
    Sequence *sequence(const QString &id);
    const Sequence *sequence(const QString &id) const;
    Bin *bin(const QString &id);
    Sequence *activeSequence();

    QJsonObject toJson() const;
    static Project fromJson(const QJsonObject &o);
    // Paths are stored relative to the project file where possible.
    void makePathsRelative(const QString &projectDir);
    void makePathsAbsolute(const QString &projectDir);
};

} // namespace rc
