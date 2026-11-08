#include "Model.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QUuid>
#include <algorithm>

namespace rc {

QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

static QString T(Ticks t) { return QString::number(t); }
static Ticks T(const QJsonValue &v, Ticks def = 0)
{
    if (v.isString()) {
        bool ok = false;
        Ticks t = v.toString().toLongLong(&ok);
        return ok ? t : def;
    }
    if (v.isDouble())
        return (Ticks)v.toDouble();
    return def;
}
static QJsonValue fr(FrameRate r) { return QJsonArray{r.num, r.den}; }
static FrameRate fr(const QJsonValue &v, FrameRate def = {25, 1})
{
    QJsonArray a = v.toArray();
    if (a.size() != 2)
        return def;
    FrameRate r{a[0].toInt(), a[1].toInt()};
    return r.valid() ? r : def;
}
template <class T, class F>
static QJsonArray arr(const std::vector<T> &v, F f)
{
    QJsonArray a;
    for (const auto &x : v)
        a.append(f(x));
    return a;
}

const std::vector<QColor> &labelColors()
{
    static const std::vector<QColor> c = {
        QColor(0x8c, 0x7a, 0xe6), QColor(0x3d, 0xa8, 0xd8), QColor(0x4c, 0xb8, 0x7a), QColor(0xe6, 0xc2, 0x3c),
        QColor(0xe6, 0x8a, 0x3c), QColor(0xd8, 0x4a, 0x4a), QColor(0xd8, 0x5a, 0xa8), QColor(0x9a, 0x9a, 0xa8),
        QColor(0x2f, 0x7a, 0x6a), QColor(0x7a, 0x52, 0x3c), QColor(0x5a, 0x6e, 0xd8), QColor(0xb8, 0xd8, 0x4c),
    };
    return c;
}
QStringList labelColorNames()
{
    return {"Violet", "Iris", "Forest", "Mango", "Tangerine", "Cerise", "Rose", "Grey",
            "Teal", "Brown", "Lavender", "Lime"};
}

// ---- MediaInfo ----------------------------------------------------------------

QJsonObject MediaInfo::toJson() const
{
    QJsonObject o;
    o["valid"] = valid;
    if (!error.isEmpty()) o["error"] = error;
    o["container"] = container;
    o["duration"] = T(duration);
    o["startTime"] = T(startTime);
    if (!timecode.isEmpty()) o["timecode"] = timecode;
    o["bitrate"] = QString::number(bitrate);
    o["hasVideo"] = hasVideo;
    o["hasAudio"] = hasAudio;
    o["isStill"] = isStill;
    o["isImageSequence"] = isImageSequence;
    o["width"] = width;
    o["height"] = height;
    o["par"] = pixelAspect;
    o["fps"] = fr(fps);
    o["vfr"] = variableFrameRate;
    o["videoCodec"] = videoCodec;
    o["pixelFormat"] = pixelFormat;
    o["bitDepth"] = bitDepth;
    o["rotation"] = rotation;
    o["primaries"] = colorPrimaries;
    o["transfer"] = colorTransfer;
    o["colorSpace"] = colorSpace;
    o["fullRange"] = fullRange;
    o["hdr"] = hdr;
    o["alpha"] = hasAlpha;
    o["interlaced"] = interlaced;
    o["audioStreams"] = audioStreams;
    o["channels"] = channels;
    o["sampleRate"] = sampleRate;
    o["audioCodec"] = audioCodec;
    return o;
}

MediaInfo MediaInfo::fromJson(const QJsonObject &o)
{
    MediaInfo m;
    m.valid = o["valid"].toBool();
    m.error = o["error"].toString();
    m.container = o["container"].toString();
    m.duration = T(o["duration"]);
    m.startTime = T(o["startTime"]);
    m.timecode = o["timecode"].toString();
    m.bitrate = o["bitrate"].toString().toLongLong();
    m.hasVideo = o["hasVideo"].toBool();
    m.hasAudio = o["hasAudio"].toBool();
    m.isStill = o["isStill"].toBool();
    m.isImageSequence = o["isImageSequence"].toBool();
    m.width = o["width"].toInt();
    m.height = o["height"].toInt();
    m.pixelAspect = o["par"].toDouble(1.0);
    m.fps = fr(o["fps"]);
    m.variableFrameRate = o["vfr"].toBool();
    m.videoCodec = o["videoCodec"].toString();
    m.pixelFormat = o["pixelFormat"].toString();
    m.bitDepth = o["bitDepth"].toInt(8);
    m.rotation = o["rotation"].toInt();
    m.colorPrimaries = o["primaries"].toString();
    m.colorTransfer = o["transfer"].toString();
    m.colorSpace = o["colorSpace"].toString();
    m.fullRange = o["fullRange"].toBool();
    m.hdr = o["hdr"].toBool();
    m.hasAlpha = o["alpha"].toBool();
    m.interlaced = o["interlaced"].toBool();
    m.audioStreams = o["audioStreams"].toInt();
    m.channels = o["channels"].toInt();
    m.sampleRate = o["sampleRate"].toInt();
    m.audioCodec = o["audioCodec"].toString();
    return m;
}

QString MediaInfo::summary() const
{
    QStringList parts;
    if (hasVideo) {
        parts << QString("%1x%2").arg(width).arg(height);
        if (!isStill)
            parts << fps.toString() + " fps";
        parts << videoCodec.toUpper();
        if (bitDepth > 8)
            parts << QString("%1-bit").arg(bitDepth);
        if (hdr)
            parts << "HDR";
    }
    if (hasAudio)
        parts << QString("%1 Hz %2 ch").arg(sampleRate).arg(channels);
    return parts.join(" · ");
}

// ---- Marker ----------------------------------------------------------------------

QJsonObject Marker::toJson() const
{
    QJsonObject o;
    o["id"] = id;
    o["time"] = T(time);
    if (duration) o["duration"] = T(duration);
    o["name"] = name;
    if (!comment.isEmpty()) o["comment"] = comment;
    o["color"] = color;
    o["type"] = int(type);
    return o;
}
Marker Marker::fromJson(const QJsonObject &o)
{
    Marker m;
    m.id = o["id"].toString();
    if (m.id.isEmpty()) m.id = newId();
    m.time = T(o["time"]);
    m.duration = T(o["duration"]);
    m.name = o["name"].toString();
    m.comment = o["comment"].toString();
    m.color = o["color"].toInt();
    m.type = Type(o["type"].toInt());
    return m;
}

// ---- MediaItem -------------------------------------------------------------------

QJsonObject MediaItem::toJson() const
{
    QJsonObject o;
    o["id"] = id;
    o["name"] = name;
    o["path"] = path;
    if (!binId.isEmpty()) o["bin"] = binId;
    if (!proxyPath.isEmpty()) o["proxy"] = proxyPath;
    o["info"] = info.toJson();
    o["label"] = label;
    if (!comment.isEmpty()) o["comment"] = comment;
    if (!scene.isEmpty()) o["scene"] = scene;
    if (!shot.isEmpty()) o["shot"] = shot;
    if (!take.isEmpty()) o["take"] = take;
    if (favourite) o["favourite"] = true;
    if (inPoint != kInvalidTicks) o["in"] = T(inPoint);
    if (outPoint != kInvalidTicks) o["out"] = T(outPoint);
    if (!markers.empty()) o["markers"] = arr(markers, [](const Marker &m) { return m.toJson(); });
    if (fpsOverride) o["fpsOverride"] = fr(*fpsOverride);
    if (alphaMode) o["alphaMode"] = alphaMode;
    if (loudnessLufs != 0) o["lufs"] = loudnessLufs;
    return o;
}

MediaItem MediaItem::fromJson(const QJsonObject &o)
{
    MediaItem m;
    m.id = o["id"].toString();
    m.name = o["name"].toString();
    m.path = o["path"].toString();
    m.binId = o["bin"].toString();
    m.proxyPath = o["proxy"].toString();
    m.info = MediaInfo::fromJson(o["info"].toObject());
    m.label = o["label"].toInt();
    m.comment = o["comment"].toString();
    m.scene = o["scene"].toString();
    m.shot = o["shot"].toString();
    m.take = o["take"].toString();
    m.favourite = o["favourite"].toBool();
    m.inPoint = o.contains("in") ? T(o["in"]) : kInvalidTicks;
    m.outPoint = o.contains("out") ? T(o["out"]) : kInvalidTicks;
    for (const auto &v : o["markers"].toArray())
        m.markers.push_back(Marker::fromJson(v.toObject()));
    if (o.contains("fpsOverride"))
        m.fpsOverride = fr(o["fpsOverride"]);
    m.alphaMode = o["alphaMode"].toInt();
    m.loudnessLufs = o["lufs"].toDouble();
    return m;
}

QJsonObject Bin::toJson() const
{
    QJsonObject o;
    o["id"] = id;
    o["name"] = name;
    if (!parentId.isEmpty()) o["parent"] = parentId;
    return o;
}
Bin Bin::fromJson(const QJsonObject &o) { return {o["id"].toString(), o["name"].toString(), o["parent"].toString()}; }

// ---- Clip ----------------------------------------------------------------------------

QString clipSourceName(ClipSource s)
{
    switch (s) {
    case ClipSource::Sequence: return "sequence";
    case ClipSource::Title: return "title";
    case ClipSource::Solid: return "solid";
    case ClipSource::Adjustment: return "adjustment";
    case ClipSource::Bars: return "bars";
    case ClipSource::Countdown: return "countdown";
    case ClipSource::Transparent: return "transparent";
    case ClipSource::Media:
    default: return "media";
    }
}
ClipSource clipSourceFromName(const QString &s)
{
    if (s == "sequence") return ClipSource::Sequence;
    if (s == "title") return ClipSource::Title;
    if (s == "solid") return ClipSource::Solid;
    if (s == "adjustment") return ClipSource::Adjustment;
    if (s == "bars") return ClipSource::Bars;
    if (s == "countdown") return ClipSource::Countdown;
    if (s == "transparent") return ClipSource::Transparent;
    return ClipSource::Media;
}

Ticks Clip::sourceAt(Ticks seqTime) const
{
    Ticks local = seqTime - start;
    if (reverse)
        local = duration - local - 1;
    return in + (Ticks)std::llround(double(local) * speed);
}

EffectInstance *Clip::effect(const QString &type)
{
    for (auto &e : effects)
        if (e.type == type)
            return &e;
    return nullptr;
}
const EffectInstance *Clip::effect(const QString &type) const
{
    for (const auto &e : effects)
        if (e.type == type)
            return &e;
    return nullptr;
}
EffectInstance *Clip::effectByUid(const QString &uid)
{
    for (auto &e : effects)
        if (e.uid == uid)
            return &e;
    return nullptr;
}

void Clip::ensureIntrinsics(TrackType t)
{
    const QStringList need = t == TrackType::Video ? QStringList{"motion", "opacity"} : QStringList{"volume", "panner"};
    int pos = 0;
    for (const auto &type : need) {
        if (!effect(type))
            effects.insert(effects.begin() + pos, EffectInstance::create(type));
        ++pos;
    }
}

QJsonObject Clip::toJson() const
{
    QJsonObject o;
    o["id"] = id;
    if (!name.isEmpty()) o["name"] = name;
    o["source"] = clipSourceName(source);
    if (!mediaId.isEmpty()) o["media"] = mediaId;
    o["start"] = T(start);
    o["duration"] = T(duration);
    o["in"] = T(in);
    if (speed != 1.0) o["speed"] = speed;
    if (reverse) o["reverse"] = true;
    if (!maintainPitch) o["maintainPitch"] = false;
    if (!enabled) o["enabled"] = false;
    if (audioStream) o["audioStream"] = audioStream;
    if (channelMap >= 0) o["channelMap"] = channelMap;
    if (!linkId.isEmpty()) o["link"] = linkId;
    if (!groupId.isEmpty()) o["group"] = groupId;
    if (label) o["label"] = label;
    o["effects"] = arr(effects, [](const EffectInstance &e) { return e.toJson(); });
    if (!gen.isEmpty()) o["gen"] = gen;
    return o;
}

Clip Clip::fromJson(const QJsonObject &o)
{
    Clip c;
    c.id = o["id"].toString();
    if (c.id.isEmpty()) c.id = newId();
    c.name = o["name"].toString();
    c.source = clipSourceFromName(o["source"].toString());
    c.mediaId = o["media"].toString();
    c.start = T(o["start"]);
    c.duration = T(o["duration"]);
    c.in = T(o["in"]);
    c.speed = o["speed"].toDouble(1.0);
    if (c.speed <= 0) c.speed = 1.0;
    c.reverse = o["reverse"].toBool();
    c.maintainPitch = o["maintainPitch"].toBool(true);
    c.enabled = o["enabled"].toBool(true);
    c.audioStream = o["audioStream"].toInt();
    c.channelMap = o["channelMap"].toInt(-1);
    c.linkId = o["link"].toString();
    c.groupId = o["group"].toString();
    c.label = o["label"].toInt();
    for (const auto &v : o["effects"].toArray())
        c.effects.push_back(EffectInstance::fromJson(v.toObject()));
    c.gen = o["gen"].toObject();
    return c;
}

// ---- Transition ------------------------------------------------------------------------

QJsonObject Transition::toJson() const
{
    QJsonObject o;
    o["id"] = id;
    if (!clipA.isEmpty()) o["a"] = clipA;
    if (!clipB.isEmpty()) o["b"] = clipB;
    o["duration"] = T(duration);
    o["align"] = int(alignment);
    o["effect"] = effect.toJson();
    return o;
}
Transition Transition::fromJson(const QJsonObject &o)
{
    Transition t;
    t.id = o["id"].toString();
    if (t.id.isEmpty()) t.id = newId();
    t.clipA = o["a"].toString();
    t.clipB = o["b"].toString();
    t.duration = T(o["duration"]);
    t.alignment = Alignment(o["align"].toInt());
    t.effect = EffectInstance::fromJson(o["effect"].toObject());
    return t;
}

// ---- Track --------------------------------------------------------------------------------

void Track::sortClips()
{
    std::stable_sort(clips.begin(), clips.end(), [](const Clip &a, const Clip &b) { return a.start < b.start; });
}
Clip *Track::clip(const QString &id)
{
    for (auto &c : clips)
        if (c.id == id)
            return &c;
    return nullptr;
}
const Clip *Track::clip(const QString &id) const
{
    for (const auto &c : clips)
        if (c.id == id)
            return &c;
    return nullptr;
}
int Track::clipIndexAt(Ticks t) const
{
    // Clips are sorted: binary search the last clip starting at or before t.
    auto it = std::upper_bound(clips.begin(), clips.end(), t, [](Ticks x, const Clip &c) { return x < c.start; });
    if (it == clips.begin())
        return -1;
    --it;
    return (t >= it->start && t < it->end()) ? int(it - clips.begin()) : -1;
}
Ticks Track::endTime() const
{
    Ticks e = 0;
    for (const auto &c : clips)
        e = std::max(e, c.end());
    return e;
}
Transition *Track::transition(const QString &id)
{
    for (auto &t : transitions)
        if (t.id == id)
            return &t;
    return nullptr;
}

QJsonObject Track::toJson() const
{
    QJsonObject o;
    o["id"] = id;
    o["name"] = name;
    o["type"] = type == TrackType::Video ? "video" : "audio";
    if (muted) o["muted"] = true;
    if (solo) o["solo"] = true;
    if (locked) o["locked"] = true;
    if (!syncLock) o["syncLock"] = false;
    if (!targeted) o["targeted"] = false;
    if (!sourcePatched) o["sourcePatched"] = false;
    if (height) o["height"] = height;
    if (channels != 2) o["channels"] = channels;
    if (volumeDb != 0) o["volume"] = volumeDb;
    if (pan != 0) o["pan"] = pan;
    o["clips"] = arr(clips, [](const Clip &c) { return c.toJson(); });
    if (!transitions.empty()) o["transitions"] = arr(transitions, [](const Transition &t) { return t.toJson(); });
    if (!effects.empty()) o["effects"] = arr(effects, [](const EffectInstance &e) { return e.toJson(); });
    return o;
}

Track Track::fromJson(const QJsonObject &o)
{
    Track t;
    t.id = o["id"].toString();
    if (t.id.isEmpty()) t.id = newId();
    t.name = o["name"].toString();
    t.type = o["type"].toString() == "audio" ? TrackType::Audio : TrackType::Video;
    t.muted = o["muted"].toBool();
    t.solo = o["solo"].toBool();
    t.locked = o["locked"].toBool();
    t.syncLock = o["syncLock"].toBool(true);
    t.targeted = o["targeted"].toBool(true);
    t.sourcePatched = o["sourcePatched"].toBool(true);
    t.height = o["height"].toInt();
    t.channels = o["channels"].toInt(2);
    t.volumeDb = o["volume"].toDouble();
    t.pan = o["pan"].toDouble();
    for (const auto &v : o["clips"].toArray()) {
        Clip c = Clip::fromJson(v.toObject());
        c.ensureIntrinsics(t.type);
        t.clips.push_back(c);
    }
    for (const auto &v : o["transitions"].toArray())
        t.transitions.push_back(Transition::fromJson(v.toObject()));
    for (const auto &v : o["effects"].toArray())
        t.effects.push_back(EffectInstance::fromJson(v.toObject()));
    t.sortClips();
    return t;
}

// ---- Captions ---------------------------------------------------------------------------

QJsonObject Caption::toJson() const
{
    QJsonObject o;
    o["id"] = id;
    o["start"] = T(start);
    o["end"] = T(end);
    o["text"] = text;
    if (!speaker.isEmpty()) o["speaker"] = speaker;
    return o;
}
Caption Caption::fromJson(const QJsonObject &o)
{
    Caption c;
    c.id = o["id"].toString();
    if (c.id.isEmpty()) c.id = newId();
    c.start = T(o["start"]);
    c.end = T(o["end"]);
    c.text = o["text"].toString();
    c.speaker = o["speaker"].toString();
    return c;
}

QJsonObject CaptionStyle::toJson() const
{
    QJsonObject o;
    o["font"] = font;
    o["size"] = size;
    o["color"] = color.name(QColor::HexArgb);
    o["outline"] = outline.name(QColor::HexArgb);
    o["outlineWidth"] = outlineWidth;
    o["background"] = background.name(QColor::HexArgb);
    o["positionY"] = positionY;
    o["bold"] = bold;
    o["uppercase"] = uppercase;
    o["maxLines"] = maxLines;
    o["maxChars"] = maxCharsPerLine;
    o["wordHighlight"] = wordHighlight;
    o["highlight"] = highlight.name(QColor::HexArgb);
    return o;
}
CaptionStyle CaptionStyle::fromJson(const QJsonObject &o)
{
    CaptionStyle s;
    if (o.isEmpty())
        return s;
    s.font = o["font"].toString(s.font);
    s.size = o["size"].toDouble(s.size);
    s.color = QColor(o["color"].toString("#ffffffff"));
    s.outline = QColor(o["outline"].toString("#ff000000"));
    s.outlineWidth = o["outlineWidth"].toDouble(s.outlineWidth);
    s.background = QColor(o["background"].toString("#00000000"));
    s.positionY = o["positionY"].toDouble(s.positionY);
    s.bold = o["bold"].toBool(s.bold);
    s.uppercase = o["uppercase"].toBool();
    s.maxLines = o["maxLines"].toInt(2);
    s.maxCharsPerLine = o["maxChars"].toInt(42);
    s.wordHighlight = o["wordHighlight"].toBool();
    s.highlight = QColor(o["highlight"].toString("#ffffdc00"));
    return s;
}

void CaptionTrack::sort()
{
    std::stable_sort(captions.begin(), captions.end(), [](const Caption &a, const Caption &b) { return a.start < b.start; });
}
QJsonObject CaptionTrack::toJson() const
{
    QJsonObject o;
    o["id"] = id;
    o["name"] = name;
    o["language"] = language;
    o["visible"] = visible;
    o["burnIn"] = burnIn;
    o["style"] = style.toJson();
    o["captions"] = arr(captions, [](const Caption &c) { return c.toJson(); });
    return o;
}
CaptionTrack CaptionTrack::fromJson(const QJsonObject &o)
{
    CaptionTrack t;
    t.id = o["id"].toString();
    if (t.id.isEmpty()) t.id = newId();
    t.name = o["name"].toString("Subtitles");
    t.language = o["language"].toString("fr");
    t.visible = o["visible"].toBool(true);
    t.burnIn = o["burnIn"].toBool();
    t.style = CaptionStyle::fromJson(o["style"].toObject());
    for (const auto &v : o["captions"].toArray())
        t.captions.push_back(Caption::fromJson(v.toObject()));
    t.sort();
    return t;
}

// ---- Sequence -----------------------------------------------------------------------------

Ticks Sequence::duration() const
{
    Ticks e = 0;
    for (const auto &t : videoTracks) e = std::max(e, t.endTime());
    for (const auto &t : audioTracks) e = std::max(e, t.endTime());
    for (const auto &ct : captionTracks)
        for (const auto &c : ct.captions) e = std::max(e, c.end);
    return e;
}

Track *Sequence::track(TrackType t, int index)
{
    auto &v = tracks(t);
    return index >= 0 && index < (int)v.size() ? &v[index] : nullptr;
}
const Track *Sequence::track(TrackType t, int index) const
{
    const auto &v = tracks(t);
    return index >= 0 && index < (int)v.size() ? &v[index] : nullptr;
}

Track *Sequence::addTrack(TrackType t)
{
    Track tr;
    tr.id = newId();
    tr.type = t;
    tracks(t).push_back(tr);
    renameTracks();
    return &tracks(t).back();
}

void Sequence::renameTracks()
{
    for (size_t i = 0; i < videoTracks.size(); ++i)
        if (videoTracks[i].name.isEmpty() || videoTracks[i].name.startsWith("Video "))
            videoTracks[i].name = QString("Video %1").arg(i + 1);
    for (size_t i = 0; i < audioTracks.size(); ++i)
        if (audioTracks[i].name.isEmpty() || audioTracks[i].name.startsWith("Audio "))
            audioTracks[i].name = QString("Audio %1").arg(i + 1);
}

void Sequence::ensureDefaultTracks()
{
    while (videoTracks.size() < 3) addTrack(TrackType::Video);
    while (audioTracks.size() < 3) addTrack(TrackType::Audio);
}

std::optional<Sequence::ClipRef> Sequence::findClip(const QString &clipId) const
{
    for (int k = 0; k < 2; ++k) {
        TrackType tt = k == 0 ? TrackType::Video : TrackType::Audio;
        const auto &v = tracks(tt);
        for (int ti = 0; ti < (int)v.size(); ++ti)
            for (int ci = 0; ci < (int)v[ti].clips.size(); ++ci)
                if (v[ti].clips[ci].id == clipId)
                    return ClipRef{tt, ti, ci};
    }
    return std::nullopt;
}
Clip *Sequence::clip(const QString &clipId)
{
    auto r = findClip(clipId);
    return r ? &tracks(r->type)[r->track].clips[r->index] : nullptr;
}
const Clip *Sequence::clip(const QString &clipId) const
{
    auto r = findClip(clipId);
    return r ? &tracks(r->type)[r->track].clips[r->index] : nullptr;
}

std::vector<QString> Sequence::linkedClips(const QString &clipId) const
{
    std::vector<QString> out;
    const Clip *c = clip(clipId);
    if (!c)
        return out;
    if (c->linkId.isEmpty()) {
        out.push_back(clipId);
        return out;
    }
    for (const auto *v : {&videoTracks, &audioTracks})
        for (const auto &t : *v)
            for (const auto &cl : t.clips)
                if (cl.linkId == c->linkId)
                    out.push_back(cl.id);
    return out;
}

TimeRange Sequence::workRange() const
{
    TimeRange r{0, duration()};
    if (inPoint != kInvalidTicks) r.start = inPoint;
    if (outPoint != kInvalidTicks) r.end = outPoint;
    if (r.end <= r.start) r = {0, duration()};
    return r;
}

QJsonObject Sequence::toJson() const
{
    QJsonObject o;
    o["id"] = id;
    o["name"] = name;
    if (!binId.isEmpty()) o["bin"] = binId;
    o["width"] = width;
    o["height"] = height;
    o["par"] = pixelAspect;
    o["fps"] = fr(fps);
    o["sampleRate"] = sampleRate;
    o["audioChannels"] = audioChannels;
    o["startTimecode"] = T(startTimecode);
    o["video"] = arr(videoTracks, [](const Track &t) { return t.toJson(); });
    o["audio"] = arr(audioTracks, [](const Track &t) { return t.toJson(); });
    if (!captionTracks.empty()) o["captions"] = arr(captionTracks, [](const CaptionTrack &t) { return t.toJson(); });
    if (!markers.empty()) o["markers"] = arr(markers, [](const Marker &m) { return m.toJson(); });
    if (!masterEffects.empty()) o["masterEffects"] = arr(masterEffects, [](const EffectInstance &e) { return e.toJson(); });
    if (masterVolumeDb != 0) o["masterVolume"] = masterVolumeDb;
    if (inPoint != kInvalidTicks) o["in"] = T(inPoint);
    if (outPoint != kInvalidTicks) o["out"] = T(outPoint);
    o["playhead"] = T(playhead);
    o["colorSpace"] = colorSpace;
    return o;
}

Sequence Sequence::fromJson(const QJsonObject &o)
{
    Sequence s;
    s.id = o["id"].toString();
    if (s.id.isEmpty()) s.id = newId();
    s.name = o["name"].toString("Sequence");
    s.binId = o["bin"].toString();
    s.width = o["width"].toInt(1920);
    s.height = o["height"].toInt(1080);
    s.pixelAspect = o["par"].toDouble(1.0);
    s.fps = fr(o["fps"]);
    s.sampleRate = o["sampleRate"].toInt(48000);
    s.audioChannels = o["audioChannels"].toInt(2);
    s.startTimecode = T(o["startTimecode"]);
    for (const auto &v : o["video"].toArray()) s.videoTracks.push_back(Track::fromJson(v.toObject()));
    for (const auto &v : o["audio"].toArray()) s.audioTracks.push_back(Track::fromJson(v.toObject()));
    for (auto &t : s.videoTracks) t.type = TrackType::Video;
    for (auto &t : s.audioTracks) t.type = TrackType::Audio;
    for (const auto &v : o["captions"].toArray()) s.captionTracks.push_back(CaptionTrack::fromJson(v.toObject()));
    for (const auto &v : o["markers"].toArray()) s.markers.push_back(Marker::fromJson(v.toObject()));
    for (const auto &v : o["masterEffects"].toArray()) s.masterEffects.push_back(EffectInstance::fromJson(v.toObject()));
    s.masterVolumeDb = o["masterVolume"].toDouble();
    s.inPoint = o.contains("in") ? T(o["in"]) : kInvalidTicks;
    s.outPoint = o.contains("out") ? T(o["out"]) : kInvalidTicks;
    s.playhead = T(o["playhead"]);
    s.colorSpace = o["colorSpace"].toString("rec709");
    return s;
}

// ---- Settings / Project ---------------------------------------------------------------------

QJsonObject ProjectSettings::toJson() const
{
    QJsonObject o;
    o["proxyHeight"] = proxyHeight;
    o["proxyCodec"] = proxyCodec;
    o["useProxies"] = useProxies;
    if (!scratchDir.isEmpty()) o["scratchDir"] = scratchDir;
    o["autosaveMinutes"] = autosaveMinutes;
    o["autosaveKeep"] = autosaveKeep;
    o["stillDuration"] = T(defaultStillDuration);
    o["videoTransition"] = T(defaultVideoTransition);
    o["audioTransition"] = T(defaultAudioTransition);
    o["videoTransitionType"] = defaultVideoTransitionType;
    o["audioTransitionType"] = defaultAudioTransitionType;
    o["playbackResolution"] = playbackResolution;
    return o;
}
ProjectSettings ProjectSettings::fromJson(const QJsonObject &o)
{
    ProjectSettings s;
    s.proxyHeight = o["proxyHeight"].toInt(540);
    s.proxyCodec = o["proxyCodec"].toString("h264");
    s.useProxies = o["useProxies"].toBool(true);
    s.scratchDir = o["scratchDir"].toString();
    s.autosaveMinutes = o["autosaveMinutes"].toInt(5);
    s.autosaveKeep = o["autosaveKeep"].toInt(20);
    s.defaultStillDuration = T(o["stillDuration"], 5 * kTicksPerSecond);
    s.defaultVideoTransition = T(o["videoTransition"], kTicksPerSecond);
    s.defaultAudioTransition = T(o["audioTransition"], kTicksPerSecond / 2);
    s.defaultVideoTransitionType = o["videoTransitionType"].toString("crossDissolve");
    s.defaultAudioTransitionType = o["audioTransitionType"].toString("constantPower");
    s.playbackResolution = o["playbackResolution"].toInt(1);
    return s;
}

MediaItem *Project::mediaItem(const QString &id)
{
    for (auto &m : media)
        if (m.id == id)
            return &m;
    return nullptr;
}
const MediaItem *Project::mediaItem(const QString &id) const
{
    for (const auto &m : media)
        if (m.id == id)
            return &m;
    return nullptr;
}
MediaItem *Project::mediaByPath(const QString &path)
{
    for (auto &m : media)
        if (m.path == path)
            return &m;
    return nullptr;
}
Sequence *Project::sequence(const QString &id)
{
    for (auto &s : sequences)
        if (s.id == id)
            return &s;
    return nullptr;
}
const Sequence *Project::sequence(const QString &id) const
{
    for (const auto &s : sequences)
        if (s.id == id)
            return &s;
    return nullptr;
}
Bin *Project::bin(const QString &id)
{
    for (auto &b : bins)
        if (b.id == id)
            return &b;
    return nullptr;
}
Sequence *Project::activeSequence()
{
    if (Sequence *s = sequence(activeSequenceId))
        return s;
    return sequences.empty() ? nullptr : &sequences.front();
}

QJsonObject Project::toJson() const
{
    QJsonObject o;
    o["format"] = "raccord-project";
    o["version"] = 1;
    o["name"] = name;
    o["media"] = arr(media, [](const MediaItem &m) { return m.toJson(); });
    o["bins"] = arr(bins, [](const Bin &b) { return b.toJson(); });
    o["sequences"] = arr(sequences, [](const Sequence &s) { return s.toJson(); });
    o["activeSequence"] = activeSequenceId;
    o["settings"] = settings.toJson();
    return o;
}

Project Project::fromJson(const QJsonObject &o)
{
    Project p;
    p.name = o["name"].toString("Untitled");
    for (const auto &v : o["media"].toArray()) p.media.push_back(MediaItem::fromJson(v.toObject()));
    for (const auto &v : o["bins"].toArray()) p.bins.push_back(Bin::fromJson(v.toObject()));
    for (const auto &v : o["sequences"].toArray()) p.sequences.push_back(Sequence::fromJson(v.toObject()));
    p.activeSequenceId = o["activeSequence"].toString();
    p.settings = ProjectSettings::fromJson(o["settings"].toObject());
    return p;
}

void Project::makePathsRelative(const QString &projectDir)
{
    QDir d(projectDir);
    for (auto &m : media) {
        if (!m.path.isEmpty() && QFileInfo(m.path).isAbsolute())
            m.path = d.relativeFilePath(m.path);
        if (!m.proxyPath.isEmpty() && QFileInfo(m.proxyPath).isAbsolute())
            m.proxyPath = d.relativeFilePath(m.proxyPath);
    }
}

void Project::makePathsAbsolute(const QString &projectDir)
{
    QDir d(projectDir);
    for (auto &m : media) {
        if (!m.path.isEmpty() && QFileInfo(m.path).isRelative())
            m.path = QDir::cleanPath(d.absoluteFilePath(m.path));
        if (!m.proxyPath.isEmpty() && QFileInfo(m.proxyPath).isRelative())
            m.proxyPath = QDir::cleanPath(d.absoluteFilePath(m.proxyPath));
    }
}

} // namespace rc
