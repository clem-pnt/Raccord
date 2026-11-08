#include "Interchange.h"
#include "core/Edit.h"
#include "media/MediaProbe.h"
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTextStream>
#include <QUrl>
#include <cmath>
#include <zlib.h>

namespace rc::interop {

// ---------------------------------------------------------------------------
// Helpers

static int timebase(FrameRate f) { return f.nominal(); }
static bool ntsc(FrameRate f) { return f.isNtsc(); }
static int64_t frames(Ticks t, FrameRate f) { return ticksToFrame(t, f); }
static Ticks fromFrames(int64_t n, FrameRate f) { return frameToTicks(n, f); }

static FrameRate rateFrom(int tb, bool isNtsc)
{
    if (isNtsc) return {tb * 1000, 1001};
    return {std::max(1, tb), 1};
}

static QString fileUrl(const QString &path)
{
    QString u = QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
    if (u.startsWith("file:///")) u = "file://localhost/" + u.mid(8);
    return u;
}

static QString pathFromUrl(QString u)
{
    if (u.startsWith("file://localhost/")) u = "file:///" + u.mid(17);
    QUrl url(u);
    if (url.isLocalFile()) return url.toLocalFile();
    // Windows/mac style paths from other machines: keep as text.
    return QUrl::fromPercentEncoding(u.toUtf8());
}

static bool writeFile(const QString &path, const QByteArray &data, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = f.errorString();
        return false;
    }
    f.write(data);
    return true;
}

// Finds or creates a media item for a file path.
static MediaItem *mediaFor(Project &p, const QString &path, const QString &name, const QString &binId = {})
{
    if (MediaItem *m = p.mediaByPath(path)) return m;
    MediaItem m;
    m.id = newId();
    m.path = path;
    m.name = name.isEmpty() ? QFileInfo(path).fileName() : name;
    m.binId = binId;
    m.info = probeMedia(path);
    if (!m.info.valid) {
        // Offline media: keep a placeholder so the edit survives relinking.
        m.info.hasVideo = true;
        m.info.hasAudio = true;
        m.info.duration = 3600 * kTicksPerSecond;
    }
    p.media.push_back(m);
    return &p.media.back();
}

static void finishSequence(Sequence &s)
{
    if (s.videoTracks.empty()) s.addTrack(TrackType::Video);
    if (s.audioTracks.empty()) s.addTrack(TrackType::Audio);
    s.renameTracks();
    for (auto *v : {&s.videoTracks, &s.audioTracks})
        for (auto &t : *v)
            for (auto &c : t.clips) c.ensureIntrinsics(t.type);
    edit::normalize(s);
}

static QDomElement child(const QDomElement &e, const QString &tag) { return e.firstChildElement(tag); }
static QString text(const QDomElement &e, const QString &tag) { return e.firstChildElement(tag).text().trimmed(); }

// ---------------------------------------------------------------------------
// FCP7 XML export

namespace {
struct XmlWriter {
    QDomDocument doc;
    const Project &project;
    QHash<QString, QString> fileIds;   // media id -> file id
    int clipCounter = 0;
    explicit XmlWriter(const Project &p) : project(p) {}

    QDomElement el(QDomElement parent, const QString &tag, const QString &value = QString())
    {
        QDomElement e = doc.createElement(tag);
        if (!value.isNull()) e.appendChild(doc.createTextNode(value));
        parent.appendChild(e);
        return e;
    }
    void rate(QDomElement parent, FrameRate f)
    {
        QDomElement r = el(parent, "rate");
        el(r, "timebase", QString::number(timebase(f)));
        el(r, "ntsc", ntsc(f) ? "TRUE" : "FALSE");
    }
    void fileElement(QDomElement parent, const MediaItem &m, FrameRate f)
    {
        QDomElement fe = el(parent, "file");
        if (fileIds.contains(m.id)) {
            fe.setAttribute("id", fileIds[m.id]);
            return;
        }
        const QString id = "file-" + QString::number(fileIds.size() + 1);
        fileIds[m.id] = id;
        fe.setAttribute("id", id);
        el(fe, "name", m.name);
        el(fe, "pathurl", fileUrl(m.path));
        rate(fe, m.info.isStill ? f : m.fps());
        el(fe, "duration", QString::number(frames(m.info.duration, m.info.isStill ? f : m.fps())));
        QDomElement media = el(fe, "media");
        if (m.info.hasVideo) {
            QDomElement v = el(media, "video");
            QDomElement sc = el(v, "samplecharacteristics");
            el(sc, "width", QString::number(m.info.width));
            el(sc, "height", QString::number(m.info.height));
        }
        if (m.info.hasAudio) {
            QDomElement a = el(media, "audio");
            QDomElement sc = el(a, "samplecharacteristics");
            el(sc, "depth", "16");
            el(sc, "samplerate", QString::number(m.info.sampleRate ? m.info.sampleRate : 48000));
            el(a, "channelcount", QString::number(std::max(1, m.info.channels)));
        }
    }
    void effect(QDomElement filter, const QString &name, const QString &id, const QString &category, const QString &type, const QString &mediatype,
                const std::vector<std::pair<QString, QString>> &params)
    {
        QDomElement e = el(filter, "effect");
        el(e, "name", name);
        el(e, "effectid", id);
        if (!category.isEmpty()) el(e, "effectcategory", category);
        el(e, "effecttype", type);
        el(e, "mediatype", mediatype);
        for (const auto &[pid, val] : params) {
            QDomElement p = el(e, "parameter");
            el(p, "parameterid", pid);
            el(p, "name", pid);
            el(p, "value", val);
        }
    }
    void sequence(QDomElement parent, const Sequence &s, int depth = 0);
};

void XmlWriter::sequence(QDomElement parent, const Sequence &s, int depth)
{
    QDomElement se = el(parent, "sequence");
    se.setAttribute("id", "sequence-" + s.id);
    el(se, "name", s.name);
    el(se, "duration", QString::number(frames(s.duration(), s.fps)));
    rate(se, s.fps);
    QDomElement tc = el(se, "timecode");
    rate(tc, s.fps);
    el(tc, "string", formatTimecode(s.startTimecode, s.fps));
    el(tc, "frame", QString::number(frames(s.startTimecode, s.fps)));
    el(tc, "displayformat", s.fps.isNtsc() ? "DF" : "NDF");
    QDomElement media = el(se, "media");
    QDomElement video = el(media, "video");
    QDomElement fmt = el(video, "format");
    QDomElement sc = el(fmt, "samplecharacteristics");
    rate(sc, s.fps);
    el(sc, "width", QString::number(s.width));
    el(sc, "height", QString::number(s.height));
    el(sc, "pixelaspectratio", "square");
    QDomElement audio = el(media, "audio");
    el(audio, "numOutputChannels", "2");

    // Assign clipitem ids first so links can reference them.
    QHash<QString, QString> itemId;
    QHash<QString, std::pair<int, int>> itemPos;   // track index (1 based), clip index (1 based)
    for (TrackType type : {TrackType::Video, TrackType::Audio}) {
        const auto &tracks = s.tracks(type);
        for (int ti = 0; ti < (int)tracks.size(); ++ti)
            for (int ci = 0; ci < (int)tracks[ti].clips.size(); ++ci) {
                itemId[tracks[ti].clips[ci].id] = "clipitem-" + QString::number(++clipCounter);
                itemPos[tracks[ti].clips[ci].id] = {ti + 1, ci + 1};
            }
    }
    for (TrackType type : {TrackType::Video, TrackType::Audio}) {
        const auto &tracks = s.tracks(type);
        const QString mt = type == TrackType::Video ? "video" : "audio";
        for (int ti = 0; ti < (int)tracks.size(); ++ti) {
            const Track &t = tracks[ti];
            QDomElement te = el(type == TrackType::Video ? video : audio, "track");
            el(te, "enabled", t.muted ? "FALSE" : "TRUE");
            el(te, "locked", t.locked ? "TRUE" : "FALSE");
            // Transitions are written between the clips, in time order.
            std::vector<std::pair<Ticks, QDomElement>> items;
            for (const auto &c : t.clips) {
                QDomElement ce = doc.createElement(c.source == ClipSource::Media || c.source == ClipSource::Sequence ? "clipitem" : "generatoritem");
                ce.setAttribute("id", itemId[c.id]);
                el(ce, "name", c.name);
                el(ce, "enabled", c.enabled ? "TRUE" : "FALSE");
                const MediaItem *m = c.source == ClipSource::Media ? project.mediaItem(c.mediaId) : nullptr;
                const Sequence *nested = c.source == ClipSource::Sequence ? project.sequence(c.mediaId) : nullptr;
                const FrameRate mfps = s.fps;
                Ticks srcLen = m ? m->info.duration : nested ? nested->duration() : c.duration;
                el(ce, "duration", QString::number(frames(Ticks(double(srcLen) / c.speed), mfps)));
                rate(ce, s.fps);
                el(ce, "start", QString::number(frames(c.start, s.fps)));
                el(ce, "end", QString::number(frames(c.end(), s.fps)));
                const int64_t in = frames(Ticks(double(c.in) / c.speed), s.fps);
                el(ce, "in", QString::number(in));
                el(ce, "out", QString::number(in + frames(c.duration, s.fps)));
                if (m) {
                    fileElement(ce, *m, s.fps);
                    if (type == TrackType::Audio) {
                        QDomElement st = el(ce, "sourcetrack");
                        el(st, "mediatype", "audio");
                        el(st, "trackindex", QString::number(c.audioStream + 1));
                    }
                } else if (nested && depth < 8) {
                    sequence(ce, *nested, depth + 1);
                } else {
                    QDomElement e = el(ce, "effect");
                    el(e, "name", c.source == ClipSource::Title ? "Text" : c.source == ClipSource::Solid ? "Color" : clipSourceName(c.source));
                    el(e, "effectid", c.source == ClipSource::Title ? "Text" : c.source == ClipSource::Solid ? "Color" : "Slug");
                    el(e, "effecttype", "generator");
                    el(e, "mediatype", "video");
                    if (c.source == ClipSource::Title) {
                        QString txt;
                        for (const auto &l : c.gen["layers"].toArray()) if (l.toObject()["type"] == "text") txt += l.toObject()["text"].toString() + "\n";
                        QDomElement p = el(e, "parameter");
                        el(p, "parameterid", "str");
                        el(p, "name", "Text");
                        el(p, "value", txt.trimmed());
                    }
                }
                QDomElement filter = el(ce, "filter");
                if (c.speed != 1.0 || c.reverse)
                    effect(filter, "Time Remap", "timeremap", "motion", "motion", "video",
                           {{"speed", QString::number(c.speed * 100 * (c.reverse ? -1 : 1))}, {"reverse", c.reverse ? "TRUE" : "FALSE"}, {"frameblending", "FALSE"}});
                if (type == TrackType::Video) {
                    if (const EffectInstance *o = c.effect("opacity"))
                        effect(filter, "Opacity", "opacity", "motion", "motion", "video", {{"opacity", QString::number(o->f("opacity", 0))}});
                    if (const EffectInstance *mo = c.effect("motion")) {
                        const QPointF pos = mo->value("position", 0).pt();
                        effect(filter, "Basic Motion", "basic", "motion", "motion", "video",
                               {{"scale", QString::number(mo->f("scale", 0))}, {"rotation", QString::number(mo->f("rotation", 0))},
                                {"center", QString("%1 %2").arg((pos.x() - 0.5) * 2).arg((pos.y() - 0.5) * 2)}});
                    }
                } else if (const EffectInstance *v = c.effect("volume")) {
                    effect(filter, "Audio Levels", "audiolevels", "audiolevels", "audiolevels", "audio",
                           {{"level", QString::number(std::pow(10.0, v->f("level", 0) / 20.0))}});
                }
                if (filter.childNodes().isEmpty()) ce.removeChild(filter);
                // Links (A/V).
                if (!c.linkId.isEmpty())
                    for (const auto &lid : s.linkedClips(c.id)) {
                        auto r = s.findClip(lid);
                        if (!r) continue;
                        QDomElement l = el(ce, "link");
                        el(l, "linkclipref", itemId[lid]);
                        el(l, "mediatype", r->type == TrackType::Video ? "video" : "audio");
                        el(l, "trackindex", QString::number(itemPos[lid].first));
                        el(l, "clipindex", QString::number(itemPos[lid].second));
                    }
                items.push_back({c.start, ce});
            }
            for (const auto &tr : t.transitions) {
                const TimeRange r = edit::transitionRange(t, tr);
                QDomElement x = doc.createElement("transitionitem");
                el(x, "start", QString::number(frames(r.start, s.fps)));
                el(x, "end", QString::number(frames(r.end, s.fps)));
                el(x, "alignment", tr.alignment == Transition::CenterAtCut ? "center" : tr.alignment == Transition::StartAtCut ? "start-black" : "end-black");
                rate(x, s.fps);
                const bool vid = type == TrackType::Video;
                QDomElement e = el(x, "effect");
                el(e, "name", vid ? "Cross Dissolve" : "Cross Fade (+3dB)");
                el(e, "effectid", vid ? "Cross Dissolve" : "KGAudioTransCrossFade3dB");
                el(e, "effectcategory", vid ? "Dissolve" : "audiotransition");
                el(e, "effecttype", "transition");
                el(e, "mediatype", mt);
                items.push_back({r.start - 1, x});
            }
            std::stable_sort(items.begin(), items.end(), [](auto &a, auto &b) { return a.first < b.first; });
            for (auto &[_, e] : items) te.appendChild(e);
        }
    }
    for (const auto &m : s.markers) {
        QDomElement me = el(se, "marker");
        el(me, "name", m.name);
        el(me, "comment", m.comment);
        el(me, "in", QString::number(frames(m.time, s.fps)));
        el(me, "out", m.duration > 0 ? QString::number(frames(m.time + m.duration, s.fps)) : "-1");
    }
}
} // namespace

bool exportFcp7Xml(const Project &project, const Sequence &seq, const QString &path, QString *error)
{
    XmlWriter w(project);
    w.doc.appendChild(w.doc.createProcessingInstruction("xml", "version=\"1.0\" encoding=\"UTF-8\""));
    w.doc.appendChild(w.doc.createComment(" DOCTYPE xmeml "));
    QDomElement root = w.doc.createElement("xmeml");
    root.setAttribute("version", "4");
    w.doc.appendChild(root);
    w.sequence(root, seq);
    return writeFile(path, w.doc.toByteArray(2), error);
}

// ---------------------------------------------------------------------------
// FCP7 XML import

namespace {
struct XmlReader {
    Project &p;
    QString baseDir;
    QHash<QString, QDomElement> files;          // file id -> element with pathurl
    QHash<QString, QString> nestedIds;          // sequence xml id -> our id
    QStringList created;
    explicit XmlReader(Project &pr) : p(pr) {}

    FrameRate rateOf(const QDomElement &e, FrameRate def)
    {
        QDomElement r = child(e, "rate");
        if (r.isNull()) return def;
        return rateFrom(text(r, "timebase").toInt(), text(r, "ntsc").toUpper() == "TRUE");
    }
    MediaItem *media(const QDomElement &fileEl, FrameRate)
    {
        QDomElement f = fileEl;
        const QString id = f.attribute("id");
        if (child(f, "pathurl").isNull() && files.contains(id)) f = files[id];
        else if (!id.isEmpty() && !child(f, "pathurl").isNull()) files[id] = f;
        QString path = pathFromUrl(text(f, "pathurl"));
        if (path.isEmpty()) return nullptr;
        if (!QFileInfo::exists(path)) {
            // Try next to the XML (moved projects).
            const QString alt = baseDir + "/" + QFileInfo(path).fileName();
            if (QFileInfo::exists(alt)) path = alt;
        }
        return mediaFor(p, path, text(f, "name"));
    }
    QString sequence(const QDomElement &se);
};

QString XmlReader::sequence(const QDomElement &se)
{
    const QString xmlId = se.attribute("id");
    if (!xmlId.isEmpty() && nestedIds.contains(xmlId)) return nestedIds[xmlId];
    Sequence s;
    s.id = newId();
    if (!xmlId.isEmpty()) nestedIds[xmlId] = s.id;
    s.name = text(se, "name");
    s.fps = rateOf(se, {25, 1});
    QDomElement mediaEl = child(se, "media");
    QDomElement sc = child(child(child(mediaEl, "video"), "format"), "samplecharacteristics");
    if (!sc.isNull()) {
        s.width = std::max(16, text(sc, "width").toInt()) & ~1;
        s.height = std::max(16, text(sc, "height").toInt()) & ~1;
    }
    QDomElement tc = child(se, "timecode");
    if (!tc.isNull()) s.startTimecode = fromFrames(text(tc, "frame").toLongLong(), s.fps);
    QHash<QString, Clip *> byXmlId;
    QHash<QString, QStringList> links;
    struct PendingTransition { TrackType type; int track; Ticks start, end; QString align; };
    std::vector<PendingTransition> transitions;
    for (TrackType type : {TrackType::Video, TrackType::Audio}) {
        QDomElement group = child(mediaEl, type == TrackType::Video ? "video" : "audio");
        int ti = 0;
        for (QDomElement te = group.firstChildElement("track"); !te.isNull(); te = te.nextSiblingElement("track"), ++ti) {
            Track *t = s.track(type, ti);
            while (!t) { s.addTrack(type); t = s.track(type, ti); }
            t->muted = text(te, "enabled").toUpper() == "FALSE";
            t->locked = text(te, "locked").toUpper() == "TRUE";
            for (QDomElement ce = te.firstChildElement(); !ce.isNull(); ce = ce.nextSiblingElement()) {
                const QString tag = ce.tagName();
                if (tag == "transitionitem") {
                    transitions.push_back({type, ti, fromFrames(text(ce, "start").toLongLong(), s.fps), fromFrames(text(ce, "end").toLongLong(), s.fps), text(ce, "alignment")});
                    continue;
                }
                if (tag != "clipitem" && tag != "generatoritem") continue;
                const int64_t start = text(ce, "start").toLongLong(), end = text(ce, "end").toLongLong();
                if (start < 0 || end <= start) continue;   // Premiere uses -1 around transitions; handled by transitionitem
                Clip c;
                c.id = newId();
                c.name = text(ce, "name");
                c.enabled = text(ce, "enabled").toUpper() != "FALSE";
                c.start = fromFrames(start, s.fps);
                c.duration = fromFrames(end - start, s.fps);
                double speed = 1.0;
                bool reverse = false;
                for (QDomElement fe = child(ce, "filter"); !fe.isNull(); fe = fe.nextSiblingElement("filter")) {
                    QDomElement e = child(fe, "effect");
                    const QString eid = text(e, "effectid");
                    for (QDomElement pe = e.firstChildElement("parameter"); !pe.isNull(); pe = pe.nextSiblingElement("parameter")) {
                        const QString pid = text(pe, "parameterid");
                        const QString val = text(pe, "value");
                        if (eid == "timeremap" && pid == "speed") { speed = std::abs(val.toDouble()) / 100.0; if (val.toDouble() < 0) reverse = true; }
                        if (eid == "timeremap" && pid == "reverse" && val.toUpper() == "TRUE") reverse = true;
                        if (eid == "opacity" && pid == "opacity") c.effects.push_back([&] { auto o = EffectInstance::create("opacity"); o.param("opacity").setStaticValue(Value(val.toDouble())); return o; }());
                        if (eid == "audiolevels" && pid == "level") {
                            auto v = EffectInstance::create("volume");
                            const double g = std::max(1e-6, val.toDouble());
                            v.param("level").setStaticValue(Value(20 * std::log10(g)));
                            c.effects.push_back(v);
                        }
                        if (eid == "basic" && pid == "scale") { auto m = EffectInstance::create("motion"); m.param("scale").setStaticValue(Value(val.toDouble())); c.effects.push_back(m); }
                    }
                }
                if (speed <= 0) speed = 1;
                c.speed = speed;
                c.reverse = reverse;
                c.in = Ticks(double(fromFrames(text(ce, "in").toLongLong(), s.fps)) * speed);
                QDomElement fileEl = child(ce, "file");
                QDomElement nested = child(ce, "sequence");
                if (!fileEl.isNull()) {
                    MediaItem *m = media(fileEl, s.fps);
                    if (!m) continue;
                    c.source = ClipSource::Media;
                    c.mediaId = m->id;
                    if (c.name.isEmpty()) c.name = m->name;
                    QDomElement st = child(ce, "sourcetrack");
                    if (type == TrackType::Audio && !st.isNull()) c.audioStream = 0;   // channels are mixed per stream
                } else if (!nested.isNull()) {
                    c.source = ClipSource::Sequence;
                    c.mediaId = sequence(nested);
                } else if (tag == "generatoritem") {
                    const QString gid = text(child(ce, "effect"), "effectid");
                    if (gid.compare("Text", Qt::CaseInsensitive) == 0 || gid.contains("text", Qt::CaseInsensitive)) {
                        c.source = ClipSource::Title;
                        QString str;
                        for (QDomElement pe = child(child(ce, "effect"), "parameter"); !pe.isNull(); pe = pe.nextSiblingElement("parameter"))
                            if (text(pe, "parameterid") == "str") str = text(pe, "value");
                        c.gen = QJsonObject{{"layers", QJsonArray{QJsonObject{{"type", "text"}, {"text", str.isEmpty() ? c.name : str}, {"font", "Inter"},
                                                                               {"size", 0.07}, {"color", "#ffffff"}, {"bold", true}, {"x", 0.5}, {"y", 0.5}, {"align", "center"}}}}};
                    } else if (gid.contains("color", Qt::CaseInsensitive)) {
                        c.source = ClipSource::Solid;
                        c.gen["color"] = "#000000";
                    } else {
                        c.source = ClipSource::Transparent;
                    }
                } else {
                    continue;
                }
                const QString xid = ce.attribute("id");
                for (QDomElement le = ce.firstChildElement("link"); !le.isNull(); le = le.nextSiblingElement("link"))
                    links[xid] << text(le, "linkclipref");
                t->clips.push_back(c);
            }
        }
    }
    // Build link ids: clips that reference each other share a link id.
    for (auto *v : {&s.videoTracks, &s.audioTracks})
        for (auto &t : *v) t.sortClips();
    {
        // Map xml ids to final clip ids by re-walking in the same order.
        QHash<QString, QString> xmlToClip;
        for (TrackType type : {TrackType::Video, TrackType::Audio}) {
            QDomElement group = child(mediaEl, type == TrackType::Video ? "video" : "audio");
            int ti = 0;
            for (QDomElement te = group.firstChildElement("track"); !te.isNull(); te = te.nextSiblingElement("track"), ++ti) {
                const Track *t = s.track(type, ti);
                if (!t) continue;
                for (QDomElement ce = te.firstChildElement(); !ce.isNull(); ce = ce.nextSiblingElement()) {
                    if (ce.tagName() != "clipitem" && ce.tagName() != "generatoritem") continue;
                    const Ticks st = fromFrames(text(ce, "start").toLongLong(), s.fps);
                    for (const auto &c : t->clips)
                        if (c.start == st) { xmlToClip[ce.attribute("id")] = c.id; break; }
                }
            }
        }
        for (auto it = links.begin(); it != links.end(); ++it) {
            std::vector<QString> ids;
            if (xmlToClip.contains(it.key())) ids.push_back(xmlToClip[it.key()]);
            for (const auto &r : it.value()) if (xmlToClip.contains(r)) ids.push_back(xmlToClip[r]);
            std::sort(ids.begin(), ids.end());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
            if (ids.size() > 1) {
                bool already = false;
                for (const auto &id : ids) if (const Clip *c = s.clip(id); c && !c->linkId.isEmpty()) already = true;
                if (!already) edit::link(s, ids);
            }
        }
    }
    finishSequence(s);
    // Transitions: find the cut inside each transition range.
    for (const auto &pt : transitions) {
        const Track *t = s.track(pt.type, pt.track);
        if (!t) continue;
        QString a, b;
        Ticks cut = pt.align == "start-black" ? pt.start : pt.align == "end-black" ? pt.end : (pt.start + pt.end) / 2;
        Ticks best = INT64_MAX;
        for (const auto &c : t->clips) {
            if (std::llabs(c.end() - cut) < best) {
                best = std::llabs(c.end() - cut);
                a = c.id;
                b.clear();
                for (const auto &o : t->clips) if (o.start == c.end()) b = o.id;
            }
        }
        if (best <= s.fps.frameTicks() * 2 + (pt.end - pt.start))
            edit::addTransition(s, {pt.type, pt.track}, a, b, pt.type == TrackType::Video ? "crossDissolve" : "constantPower", std::max(s.fps.frameTicks(), pt.end - pt.start));
    }
    for (QDomElement me = se.firstChildElement("marker"); !me.isNull(); me = me.nextSiblingElement("marker")) {
        Marker m;
        m.id = newId();
        m.name = text(me, "name");
        m.comment = text(me, "comment");
        m.time = fromFrames(text(me, "in").toLongLong(), s.fps);
        const int64_t out = text(me, "out").toLongLong();
        if (out > 0) m.duration = fromFrames(out, s.fps) - m.time;
        s.markers.push_back(m);
    }
    p.sequences.push_back(s);
    created << s.id;
    return s.id;
}
} // namespace

QStringList importFcp7Xml(const QString &path, Project &project, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return {};
    }
    QDomDocument doc;
    QString msg;
    int line = 0;
    if (!doc.setContent(&f, &msg, &line)) {
        if (error) *error = QObject::tr("XML error line %1: %2").arg(line).arg(msg);
        return {};
    }
    QDomElement root = doc.documentElement();
    if (root.tagName() != "xmeml") {
        if (error) *error = QObject::tr("Not a Final Cut Pro 7 XML file");
        return {};
    }
    XmlReader r(project);
    r.baseDir = QFileInfo(path).absolutePath();
    // Index file definitions anywhere in the document (references can come before definitions).
    QDomNodeList fl = doc.elementsByTagName("file");
    for (int i = 0; i < fl.size(); ++i) {
        QDomElement fe = fl.at(i).toElement();
        if (!child(fe, "pathurl").isNull()) r.files[fe.attribute("id")] = fe;
    }
    QStringList top;
    std::function<void(const QDomElement &)> walk = [&](const QDomElement &e) {
        for (QDomElement c = e.firstChildElement(); !c.isNull(); c = c.nextSiblingElement()) {
            if (c.tagName() == "sequence") top << r.sequence(c);
            else if (c.tagName() == "bin" || c.tagName() == "children") walk(c);
            else if (c.tagName() == "clip") {
                // Master clips in bins: import media.
                QDomElement fe = c.elementsByTagName("file").at(0).toElement();
                if (!fe.isNull()) r.media(fe, {25, 1});
            }
        }
    };
    walk(root);
    if (top.isEmpty() && error) *error = QObject::tr("No sequence found");
    return top;
}

// ---------------------------------------------------------------------------
// EDL (CMX 3600)

bool exportEdl(const Project &project, const Sequence &seq, const QString &path, QString *error)
{
    QString out;
    QTextStream ts(&out);
    ts << "TITLE: " << seq.name << "\n";
    ts << "FCM: " << (seq.fps.isNtsc() ? "DROP FRAME" : "NON-DROP FRAME") << "\n\n";
    int n = 0;
    auto tc = [&](Ticks t) { return formatTimecode(t, seq.fps); };
    auto event = [&](const Clip &c, const QString &chan, const Transition *tr, const Clip *from) {
        const MediaItem *m = project.mediaItem(c.mediaId);
        QString reel = m ? QFileInfo(m->path).completeBaseName().left(8).toUpper().replace(QRegularExpression("[^A-Z0-9_]"), "_") : "BL";
        if (reel.isEmpty()) reel = "AX";
        const Ticks srcIn = c.in, srcOut = c.sourceOut();
        const Ticks recIn = c.start + seq.startTimecode, recOut = c.end() + seq.startTimecode;
        if (tr && from) {
            // Dissolve: event starts with the outgoing source, then D xxx.
            const MediaItem *fm = project.mediaItem(from->mediaId);
            QString freel = fm ? QFileInfo(fm->path).completeBaseName().left(8).toUpper() : "BL";
            ts << QString("%1  %2 %3 C        %4 %5 %6 %7\n").arg(++n, 3, 10, QChar('0')).arg(freel, -8).arg(chan, -5)
                      .arg(tc(from->sourceOut()), tc(from->sourceOut()), tc(recIn), tc(recIn));
            ts << QString("%1  %2 %3 D    %4 %5 %6 %7 %8\n").arg(n, 3, 10, QChar('0')).arg(reel, -8).arg(chan, -5)
                      .arg(frames(tr->duration, seq.fps), 3, 10, QChar('0')).arg(tc(srcIn), tc(srcOut), tc(recIn), tc(recOut));
        } else {
            ts << QString("%1  %2 %3 C        %4 %5 %6 %7\n").arg(++n, 3, 10, QChar('0')).arg(reel, -8).arg(chan, -5)
                      .arg(tc(srcIn), tc(srcOut), tc(recIn), tc(recOut));
        }
        if (c.speed != 1.0 || c.reverse)
            ts << QString("M2   %1       %2 %3\n").arg(reel, -8).arg(c.speed * seq.fps.fps() * (c.reverse ? -1 : 1), 5, 'f', 1).arg(tc(srcIn));
        ts << "* FROM CLIP NAME: " << c.name << "\n";
        if (m) ts << "* SOURCE FILE: " << m->path << "\n";
        ts << "\n";
    };
    // EDL has a single video track: write V1 (and lower tracks for completeness, each as its own block).
    for (int ti = 0; ti < (int)seq.videoTracks.size(); ++ti) {
        const Track &t = seq.videoTracks[ti];
        if (ti > 0 && !t.clips.empty()) ts << "* VIDEO TRACK V" << (ti + 1) << "\n";
        for (const auto &c : t.clips) {
            if (c.source != ClipSource::Media) continue;
            const Transition *tr = nullptr;
            const Clip *from = nullptr;
            for (const auto &x : t.transitions) if (x.clipB == c.id && !x.clipA.isEmpty()) { tr = &x; from = seq.clip(x.clipA); }
            // Linked audio on A1 at the same time = "B" (video + audio).
            bool both = false;
            for (const auto &l : seq.linkedClips(c.id)) {
                auto r = seq.findClip(l);
                if (r && r->type == TrackType::Audio && r->track == 0) both = true;
            }
            event(c, ti == 0 && both ? "B" : "V", tr, from);
        }
    }
    for (int ti = 0; ti < (int)seq.audioTracks.size() && ti < 4; ++ti) {
        for (const auto &c : seq.audioTracks[ti].clips) {
            if (c.source != ClipSource::Media) continue;
            bool covered = false;
            if (ti == 0)
                for (const auto &l : seq.linkedClips(c.id)) {
                    auto r = seq.findClip(l);
                    if (r && r->type == TrackType::Video && r->track == 0) covered = true;
                }
            if (!covered) event(c, "A" + (ti > 0 ? QString::number(ti + 1) : QString()), nullptr, nullptr);
        }
    }
    return writeFile(path, out.toUtf8(), error);
}

QStringList importEdl(const QString &path, Project &project, FrameRate fps, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = f.errorString();
        return {};
    }
    Sequence s;
    s.id = newId();
    s.name = QFileInfo(path).completeBaseName();
    s.fps = fps;
    s.ensureDefaultTracks();
    const QString dir = QFileInfo(path).absolutePath();
    struct Ev { QString reel, chan, type; int transFrames = 0; Ticks srcIn = 0, srcOut = 0, recIn = 0, recOut = 0; QString clipName, file; double m2 = 0; };
    std::vector<Ev> events;
    QRegularExpression evRe(R"(^(\d{3,6})\s+(\S+)\s+(\S+)\s+(C|D|W\d+|K\s*B?)\s*(\d+)?\s+(\d\d:\d\d:\d\d[:;]\d\d)\s+(\d\d:\d\d:\d\d[:;]\d\d)\s+(\d\d:\d\d:\d\d[:;]\d\d)\s+(\d\d:\d\d:\d\d[:;]\d\d))");
    QRegularExpression m2Re(R"(^M2\s+(\S+)\s+(-?[\d.]+)\s+(\d\d:\d\d:\d\d[:;]\d\d))");
    Ticks recStart = -1;
    QTextStream ts(&f);
    while (!ts.atEnd()) {
        const QString line = ts.readLine().trimmed();
        if (line.startsWith("TITLE:")) { s.name = line.mid(6).trimmed(); continue; }
        auto m = evRe.match(line);
        if (m.hasMatch()) {
            Ev e;
            e.reel = m.captured(2);
            e.chan = m.captured(3);
            e.type = m.captured(4);
            e.transFrames = m.captured(5).toInt();
            e.srcIn = parseTimecode(m.captured(6), fps);
            e.srcOut = parseTimecode(m.captured(7), fps);
            e.recIn = parseTimecode(m.captured(8), fps);
            e.recOut = parseTimecode(m.captured(9), fps);
            if (recStart < 0 || e.recIn < recStart) recStart = e.recIn;
            events.push_back(e);
            continue;
        }
        auto mm = m2Re.match(line);
        if (mm.hasMatch() && !events.empty()) { events.back().m2 = mm.captured(2).toDouble(); continue; }
        if (events.empty()) continue;
        if (line.startsWith("* FROM CLIP NAME:")) events.back().clipName = line.mid(17).trimmed();
        else if (line.startsWith("* SOURCE FILE:")) events.back().file = line.mid(14).trimmed();
    }
    // Record timecode usually starts at 01:00:00:00: keep it as the sequence start timecode.
    if (recStart > 0) s.startTimecode = floorToFrame(recStart, fps);
    for (const auto &e : events) {
        if (e.recOut <= e.recIn) continue;
        if (e.reel == "BL" || e.reel == "BLACK") continue;
        QString file = e.file;
        if (file.isEmpty() || !QFileInfo::exists(file)) {
            for (const QString &cand : {e.file, dir + "/" + QFileInfo(e.file).fileName(), dir + "/" + e.clipName, dir + "/" + e.reel})
                if (!cand.isEmpty() && QFileInfo(cand).isFile()) { file = cand; break; }
        }
        if (file.isEmpty()) file = dir + "/" + (e.clipName.isEmpty() ? e.reel : e.clipName);
        MediaItem *m = mediaFor(project, file, e.clipName);
        const bool video = e.chan.contains('V') || e.chan == "B";
        const bool audio = e.chan.startsWith('A') || e.chan == "B" || e.chan.contains("A");
        const QString link = video && audio ? newId() : QString();
        Clip c;
        c.source = ClipSource::Media;
        c.mediaId = m->id;
        c.name = e.clipName.isEmpty() ? m->name : e.clipName;
        c.start = e.recIn - s.startTimecode;
        c.duration = e.recOut - e.recIn;
        c.in = e.srcIn;
        if (e.m2 != 0) {
            c.speed = std::abs(e.m2) / fps.fps();
            c.reverse = e.m2 < 0;
        } else if (e.srcOut > e.srcIn) {
            c.speed = double(e.srcOut - e.srcIn) / double(c.duration);
        }
        c.linkId = link;
        if (video) {
            Clip v = c;
            v.id = newId();
            s.videoTracks[0].clips.push_back(v);
            if (e.type == "D" && e.transFrames > 0) {
                // Dissolve into this clip.
                Transition tr;
                tr.id = newId();
                tr.clipB = v.id;
                tr.duration = fromFrames(e.transFrames, fps);
                tr.alignment = Transition::StartAtCut;
                tr.effect = EffectInstance::create("crossDissolve");
                s.videoTracks[0].transitions.push_back(tr);
            }
        }
        if (audio && m->info.hasAudio) {
            int at = 0;
            if (e.chan.size() > 1 && e.chan[0] == 'A' && e.chan[1].isDigit()) at = std::max(0, e.chan.mid(1).toInt() - 1);
            while ((int)s.audioTracks.size() <= at) s.addTrack(TrackType::Audio);
            Clip a = c;
            a.id = newId();
            s.audioTracks[at].clips.push_back(a);
        }
    }
    // Resolve transition clipA from the preceding clip.
    for (auto &t : s.videoTracks) {
        t.sortClips();
        for (auto &tr : t.transitions)
            for (const auto &c : t.clips)
                if (c.id == tr.clipB)
                    for (const auto &o : t.clips) if (o.end() == c.start) tr.clipA = o.id;
    }
    finishSequence(s);
    project.sequences.push_back(s);
    return {s.id};
}

// ---------------------------------------------------------------------------
// OpenTimelineIO

static QJsonObject rt(Ticks t, FrameRate f)
{
    return QJsonObject{{"OTIO_SCHEMA", "RationalTime.1"}, {"rate", f.fps()}, {"value", double(frames(t, f))}};
}

static QJsonObject trange(Ticks start, Ticks dur, FrameRate f)
{
    return QJsonObject{{"OTIO_SCHEMA", "TimeRange.1"}, {"start_time", rt(start, f)}, {"duration", rt(dur, f)}};
}

static Ticks fromRt(const QJsonObject &o)
{
    const double rate = o["rate"].toDouble(25);
    return secondsToTicks(o["value"].toDouble() / (rate > 0 ? rate : 25));
}

bool exportOtio(const Project &project, const Sequence &seq, const QString &path, QString *error)
{
    const FrameRate f = seq.fps;
    QJsonArray tracks;
    for (TrackType type : {TrackType::Video, TrackType::Audio}) {
        const auto &list = seq.tracks(type);
        for (int ti = 0; ti < (int)list.size(); ++ti) {
            const Track &t = list[ti];
            QJsonArray children;
            Ticks pos = 0;
            for (const auto &c : t.clips) {
                if (c.start > pos) {
                    children.append(QJsonObject{{"OTIO_SCHEMA", "Gap.1"}, {"name", ""}, {"source_range", trange(0, c.start - pos, f)}, {"effects", QJsonArray()}, {"markers", QJsonArray()}, {"metadata", QJsonObject()}});
                }
                // Transition entering this clip.
                for (const auto &tr : t.transitions)
                    if (tr.clipB == c.id && !tr.clipA.isEmpty()) {
                        const TimeRange r = edit::transitionRange(t, tr);
                        children.append(QJsonObject{{"OTIO_SCHEMA", "Transition.1"}, {"name", tr.effect.type}, {"transition_type", "SMPTE_Dissolve"},
                                                    {"in_offset", rt(c.start - r.start, f)}, {"out_offset", rt(r.end - c.start, f)}, {"metadata", QJsonObject()}});
                    }
                QJsonObject clip{{"OTIO_SCHEMA", "Clip.2"}, {"name", c.name}, {"source_range", trange(c.in, Ticks(double(c.duration) * c.speed), f)},
                                 {"markers", QJsonArray()}, {"enabled", c.enabled}};
                QJsonArray effects;
                if (c.speed != 1.0 || c.reverse)
                    effects.append(QJsonObject{{"OTIO_SCHEMA", "LinearTimeWarp.1"}, {"name", ""}, {"effect_name", "LinearTimeWarp"}, {"time_scalar", c.speed * (c.reverse ? -1 : 1)}, {"metadata", QJsonObject()}});
                clip["effects"] = effects;
                const MediaItem *m = project.mediaItem(c.mediaId);
                if (c.source == ClipSource::Media && m) {
                    QJsonObject refs{{"DEFAULT_MEDIA", QJsonObject{{"OTIO_SCHEMA", "ExternalReference.1"}, {"name", m->name},
                                                                   {"target_url", QUrl::fromLocalFile(m->path).toString()},
                                                                   {"available_range", trange(0, m->info.duration, f)}, {"metadata", QJsonObject()}}}};
                    clip["media_references"] = refs;
                    clip["active_media_reference_key"] = "DEFAULT_MEDIA";
                } else {
                    clip["media_references"] = QJsonObject{{"DEFAULT_MEDIA", QJsonObject{{"OTIO_SCHEMA", "GeneratorReference.1"}, {"name", clipSourceName(c.source)},
                                                                                          {"generator_kind", clipSourceName(c.source)}, {"parameters", QJsonObject()}, {"metadata", QJsonObject()}}}};
                    clip["active_media_reference_key"] = "DEFAULT_MEDIA";
                }
                clip["metadata"] = QJsonObject{{"raccord", QJsonObject{{"linkId", c.linkId}, {"clip", c.toJson()}}}};
                children.append(clip);
                pos = c.end();
            }
            tracks.append(QJsonObject{{"OTIO_SCHEMA", "Track.1"}, {"name", t.name}, {"kind", type == TrackType::Video ? "Video" : "Audio"},
                                      {"children", children}, {"effects", QJsonArray()}, {"markers", QJsonArray()}, {"enabled", !t.muted}, {"metadata", QJsonObject()}});
        }
    }
    QJsonArray markers;
    for (const auto &m : seq.markers)
        markers.append(QJsonObject{{"OTIO_SCHEMA", "Marker.2"}, {"name", m.name}, {"comment", m.comment}, {"color", "RED"},
                                   {"marked_range", trange(m.time, m.duration, f)}, {"metadata", QJsonObject()}});
    QJsonObject root{{"OTIO_SCHEMA", "Timeline.1"}, {"name", seq.name}, {"global_start_time", rt(seq.startTimecode, f)},
                     {"tracks", QJsonObject{{"OTIO_SCHEMA", "Stack.1"}, {"name", "tracks"}, {"children", tracks}, {"effects", QJsonArray()}, {"markers", markers}, {"metadata", QJsonObject()}}},
                     {"metadata", QJsonObject{{"raccord", QJsonObject{{"width", seq.width}, {"height", seq.height}, {"fpsNum", f.num}, {"fpsDen", f.den}}}}}};
    return writeFile(path, QJsonDocument(root).toJson(QJsonDocument::Indented), error);
}

QStringList importOtio(const QString &path, Project &project, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return {};
    }
    QJsonParseError pe;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll(), &pe).object();
    if (root.isEmpty() || !root["OTIO_SCHEMA"].toString().startsWith("Timeline")) {
        if (error) *error = QObject::tr("Not an OpenTimelineIO timeline: %1").arg(pe.errorString());
        return {};
    }
    Sequence s;
    s.id = newId();
    s.name = root["name"].toString(QFileInfo(path).completeBaseName());
    const QJsonObject meta = root["metadata"].toObject()["raccord"].toObject();
    if (!meta.isEmpty()) {
        s.width = meta["width"].toInt(1920);
        s.height = meta["height"].toInt(1080);
        s.fps = {meta["fpsNum"].toInt(25), meta["fpsDen"].toInt(1)};
    } else {
        // Rate from the first rational time found.
        const double r = root["global_start_time"].toObject()["rate"].toDouble(25);
        s.fps = FrameRate::fromDouble(r > 0 ? r : 25);
    }
    s.startTimecode = fromRt(root["global_start_time"].toObject());
    const QString dir = QFileInfo(path).absolutePath();
    int vi = 0, ai = 0;
    for (const auto &tv : root["tracks"].toObject()["children"].toArray()) {
        const QJsonObject to = tv.toObject();
        const bool video = to["kind"].toString() != "Audio";
        const TrackType type = video ? TrackType::Video : TrackType::Audio;
        const int index = video ? vi++ : ai++;
        while ((int)s.tracks(type).size() <= index) s.addTrack(type);
        Track &t = s.tracks(type)[index];
        if (!to["name"].toString().isEmpty()) t.name = to["name"].toString();
        t.muted = !to["enabled"].toBool(true);
        Ticks pos = 0;
        QJsonObject pendingTransition;
        for (const auto &cv : to["children"].toArray()) {
            const QJsonObject co = cv.toObject();
            const QString schema = co["OTIO_SCHEMA"].toString();
            const QJsonObject sr = co["source_range"].toObject();
            if (schema.startsWith("Transition")) { pendingTransition = co; continue; }
            const Ticks dur = fromRt(sr["duration"].toObject());
            if (schema.startsWith("Gap")) { pos += dur; continue; }
            if (!schema.startsWith("Clip")) { pos += dur; continue; }
            Clip c;
            const QJsonObject rmeta = co["metadata"].toObject()["raccord"].toObject();
            if (rmeta.contains("clip")) c = Clip::fromJson(rmeta["clip"].toObject());
            c.id = newId();
            c.name = co["name"].toString();
            c.enabled = co["enabled"].toBool(true);
            c.in = fromRt(sr["start_time"].toObject());
            double scalar = 1;
            for (const auto &ev : co["effects"].toArray())
                if (ev.toObject()["OTIO_SCHEMA"].toString().startsWith("LinearTimeWarp")) scalar = ev.toObject()["time_scalar"].toDouble(1);
            c.speed = std::max(0.01, std::abs(scalar));
            c.reverse = scalar < 0;
            c.start = pos;
            c.duration = floorToFrame(Ticks(double(dur) / c.speed), s.fps);
            QJsonObject ref = co["media_reference"].toObject();
            if (ref.isEmpty()) ref = co["media_references"].toObject()[co["active_media_reference_key"].toString("DEFAULT_MEDIA")].toObject();
            const QString url = ref["target_url"].toString();
            if (!url.isEmpty()) {
                QString p = QUrl(url).isLocalFile() ? QUrl(url).toLocalFile() : url;
                if (QFileInfo(p).isRelative()) p = dir + "/" + p;
                MediaItem *m = mediaFor(project, p, ref["name"].toString());
                c.source = ClipSource::Media;
                c.mediaId = m->id;
                if (c.name.isEmpty()) c.name = m->name;
            } else if (c.source == ClipSource::Media) {
                pos += c.duration;
                continue;
            }
            c.linkId = rmeta["linkId"].toString();
            t.clips.push_back(c);
            if (!pendingTransition.isEmpty() && t.clips.size() >= 2) {
                const Ticks inOff = fromRt(pendingTransition["in_offset"].toObject()), outOff = fromRt(pendingTransition["out_offset"].toObject());
                Transition tr;
                tr.id = newId();
                tr.clipA = t.clips[t.clips.size() - 2].id;
                tr.clipB = c.id;
                tr.duration = std::max(s.fps.frameTicks(), inOff + outOff);
                tr.alignment = inOff == 0 ? Transition::StartAtCut : outOff == 0 ? Transition::EndAtCut : Transition::CenterAtCut;
                tr.effect = EffectInstance::create(video ? "crossDissolve" : "constantPower");
                t.transitions.push_back(tr);
            }
            pendingTransition = {};
            pos += c.duration;
        }
    }
    for (const auto &mv : root["tracks"].toObject()["markers"].toArray()) {
        const QJsonObject mo = mv.toObject();
        Marker m;
        m.id = newId();
        m.name = mo["name"].toString();
        m.comment = mo["comment"].toString();
        m.time = fromRt(mo["marked_range"].toObject()["start_time"].toObject());
        m.duration = fromRt(mo["marked_range"].toObject()["duration"].toObject());
        s.markers.push_back(m);
    }
    // Link ids from metadata are per file: keep them (they are unique ids).
    finishSequence(s);
    project.sequences.push_back(s);
    return {s.id};
}

// ---------------------------------------------------------------------------
// FCPXML 1.9 (export)

static QString fcpTime(Ticks t, FrameRate f)
{
    // Rational seconds in frame units: frames * den / num.
    const int64_t n = frames(t, f);
    if (n == 0) return "0s";
    return QString("%1/%2s").arg(n * f.den).arg(f.num);
}

bool exportFcpxml(const Project &project, const Sequence &seq, const QString &path, QString *error)
{
    QDomDocument doc;
    doc.appendChild(doc.createProcessingInstruction("xml", "version=\"1.0\" encoding=\"UTF-8\""));
    QDomElement root = doc.createElement("fcpxml");
    root.setAttribute("version", "1.9");
    doc.appendChild(root);
    QDomElement res = doc.createElement("resources");
    root.appendChild(res);
    QDomElement fmt = doc.createElement("format");
    fmt.setAttribute("id", "r1");
    fmt.setAttribute("frameDuration", QString("%1/%2s").arg(seq.fps.den).arg(seq.fps.num));
    fmt.setAttribute("width", seq.width);
    fmt.setAttribute("height", seq.height);
    res.appendChild(fmt);
    QHash<QString, QString> assets;
    int rid = 2;
    for (const auto *v : {&seq.videoTracks, &seq.audioTracks})
        for (const auto &t : *v)
            for (const auto &c : t.clips) {
                const MediaItem *m = c.source == ClipSource::Media ? project.mediaItem(c.mediaId) : nullptr;
                if (!m || assets.contains(m->id)) continue;
                const QString id = "r" + QString::number(rid++);
                assets[m->id] = id;
                QDomElement a = doc.createElement("asset");
                a.setAttribute("id", id);
                a.setAttribute("name", m->name);
                a.setAttribute("start", "0s");
                a.setAttribute("duration", fcpTime(m->info.duration, seq.fps));
                a.setAttribute("hasVideo", m->info.hasVideo ? "1" : "0");
                a.setAttribute("hasAudio", m->info.hasAudio ? "1" : "0");
                a.setAttribute("format", "r1");
                QDomElement mr = doc.createElement("media-rep");
                mr.setAttribute("kind", "original-media");
                mr.setAttribute("src", QUrl::fromLocalFile(m->path).toString());
                a.appendChild(mr);
                res.appendChild(a);
            }
    QDomElement lib = doc.createElement("library");
    root.appendChild(lib);
    QDomElement ev = doc.createElement("event");
    ev.setAttribute("name", project.name);
    lib.appendChild(ev);
    QDomElement proj = doc.createElement("project");
    proj.setAttribute("name", seq.name);
    ev.appendChild(proj);
    QDomElement se = doc.createElement("sequence");
    se.setAttribute("format", "r1");
    se.setAttribute("duration", fcpTime(seq.duration(), seq.fps));
    se.setAttribute("tcStart", fcpTime(seq.startTimecode, seq.fps));
    se.setAttribute("tcFormat", seq.fps.isNtsc() ? "DF" : "NDF");
    proj.appendChild(se);
    QDomElement spine = doc.createElement("spine");
    se.appendChild(spine);
    // One gap spanning the sequence; every clip is a connected clip on its own lane.
    QDomElement gap = doc.createElement("gap");
    gap.setAttribute("name", "Gap");
    gap.setAttribute("offset", "0s");
    gap.setAttribute("start", "0s");
    gap.setAttribute("duration", fcpTime(std::max(seq.duration(), seq.fps.frameTicks()), seq.fps));
    spine.appendChild(gap);
    for (TrackType type : {TrackType::Video, TrackType::Audio}) {
        const auto &tracks = seq.tracks(type);
        for (int ti = 0; ti < (int)tracks.size(); ++ti)
            for (const auto &c : tracks[ti].clips) {
                const MediaItem *m = c.source == ClipSource::Media ? project.mediaItem(c.mediaId) : nullptr;
                QDomElement ce;
                if (m) {
                    ce = doc.createElement("asset-clip");
                    ce.setAttribute("ref", assets[m->id]);
                    if (type == TrackType::Audio) ce.setAttribute("audioRole", "dialogue");
                    if (type == TrackType::Video && m->info.hasAudio) ce.setAttribute("srcEnable", "video");
                    if (type == TrackType::Audio && m->info.hasVideo) ce.setAttribute("srcEnable", "audio");
                } else if (c.source == ClipSource::Title) {
                    ce = doc.createElement("title");
                    QString txt;
                    for (const auto &l : c.gen["layers"].toArray()) if (l.toObject()["type"] == "text") txt += l.toObject()["text"].toString() + " ";
                    QDomElement te = doc.createElement("text");
                    te.appendChild(doc.createTextNode(txt.trimmed()));
                    ce.appendChild(te);
                } else {
                    continue;
                }
                ce.setAttribute("name", c.name);
                ce.setAttribute("lane", type == TrackType::Video ? ti + 1 : -(ti + 1));
                ce.setAttribute("offset", fcpTime(c.start, seq.fps));
                ce.setAttribute("start", fcpTime(Ticks(double(c.in) / c.speed), seq.fps));
                ce.setAttribute("duration", fcpTime(c.duration, seq.fps));
                if (!c.enabled) ce.setAttribute("enabled", "0");
                if (c.speed != 1.0 || c.reverse) {
                    QDomElement tm = doc.createElement("timeMap");
                    QDomElement p0 = doc.createElement("timept"), p1 = doc.createElement("timept");
                    p0.setAttribute("time", "0s");
                    p0.setAttribute("value", fcpTime(c.reverse ? c.sourceOut() : c.in, seq.fps));
                    p0.setAttribute("interp", "linear");
                    p1.setAttribute("time", fcpTime(c.duration, seq.fps));
                    p1.setAttribute("value", fcpTime(c.reverse ? c.in : c.sourceOut(), seq.fps));
                    p1.setAttribute("interp", "linear");
                    tm.appendChild(p0);
                    tm.appendChild(p1);
                    ce.appendChild(tm);
                }
                if (type == TrackType::Audio)
                    if (const EffectInstance *v = c.effect("volume"); v && v->f("level", 0) != 0) {
                        QDomElement av = doc.createElement("adjust-volume");
                        av.setAttribute("amount", QString("%1dB").arg(v->f("level", 0), 0, 'f', 1));
                        ce.appendChild(av);
                    }
                gap.appendChild(ce);
            }
    }
    for (const auto &mk : seq.markers) {
        QDomElement me = doc.createElement("marker");
        me.setAttribute("start", fcpTime(mk.time, seq.fps));
        me.setAttribute("duration", fcpTime(std::max(mk.duration, seq.fps.frameTicks()), seq.fps));
        me.setAttribute("value", mk.name);
        gap.appendChild(me);
    }
    return writeFile(path, doc.toByteArray(2), error);
}

// ---------------------------------------------------------------------------
// Premiere Pro .prproj (gzip compressed XML) import

static QByteArray gunzip(const QByteArray &in)
{
    if (in.size() < 2 || uint8_t(in[0]) != 0x1f || uint8_t(in[1]) != 0x8b) return in;   // plain XML
    z_stream zs{};
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) return {};
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.data()));
    zs.avail_in = uInt(in.size());
    QByteArray out;
    char buf[1 << 16];
    int ret;
    do {
        zs.next_out = reinterpret_cast<Bytef *>(buf);
        zs.avail_out = sizeof(buf);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) { inflateEnd(&zs); return {}; }
        out.append(buf, int(sizeof(buf) - zs.avail_out));
    } while (ret != Z_STREAM_END);
    inflateEnd(&zs);
    return out;
}

namespace {
struct PrReader {
    QHash<QString, QDomElement> byId, byUid;
    QDomElement ref(const QDomElement &e) const
    {
        if (e.isNull()) return {};
        if (e.hasAttribute("ObjectRef")) return byId.value(e.attribute("ObjectRef"));
        if (e.hasAttribute("ObjectURef")) return byUid.value(e.attribute("ObjectURef"));
        return {};
    }
    // First descendant with the given tag (depth-first).
    static QDomElement find(const QDomElement &e, const QString &tag)
    {
        if (e.isNull()) return {};
        QDomNodeList l = e.elementsByTagName(tag);
        return l.isEmpty() ? QDomElement() : l.at(0).toElement();
    }
    QDomElement follow(const QDomElement &e, const QString &tag) const
    {
        QDomElement c = find(e, tag);
        QDomElement r = ref(c);
        return r.isNull() ? c : r;
    }
};
} // namespace

QStringList importPrproj(const QString &path, Project &project, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return {};
    }
    const QByteArray xml = gunzip(f.readAll());
    QDomDocument doc;
    QString msg;
    if (xml.isEmpty() || !doc.setContent(xml, &msg)) {
        if (error) *error = QObject::tr("Cannot read the Premiere Pro project: %1").arg(msg);
        return {};
    }
    PrReader r;
    // Index every object.
    std::function<void(const QDomElement &)> index = [&](const QDomElement &e) {
        for (QDomElement c = e.firstChildElement(); !c.isNull(); c = c.nextSiblingElement()) {
            if (c.hasAttribute("ObjectID")) r.byId[c.attribute("ObjectID")] = c;
            if (c.hasAttribute("ObjectUID")) r.byUid[c.attribute("ObjectUID")] = c;
        }
    };
    index(doc.documentElement());
    const QString dir = QFileInfo(path).absolutePath();
    auto mediaPath = [&](const QDomElement &media) {
        QString p = PrReader::find(media, "ActualMediaFilePath").text();
        if (p.isEmpty()) p = PrReader::find(media, "FilePath").text();
        if (!p.isEmpty() && !QFileInfo::exists(p)) {
            const QString alt = dir + "/" + QFileInfo(QString(p).replace('\\', '/')).fileName();
            if (QFileInfo::exists(alt)) p = alt;
        }
        return p;
    };
    QStringList created;
    // Media used anywhere.
    for (auto it = r.byUid.begin(); it != r.byUid.end(); ++it)
        if (it.value().tagName() == "Media") {
            const QString p = mediaPath(it.value());
            if (!p.isEmpty() && QFileInfo::exists(p)) mediaFor(project, p, PrReader::find(it.value(), "Title").text());
        }
    // Sequences.
    for (auto it = r.byUid.begin(); it != r.byUid.end(); ++it) {
        const QDomElement sq = it.value();
        if (sq.tagName() != "Sequence") continue;
        Sequence s;
        s.id = newId();
        s.name = PrReader::find(sq, "Name").text();
        if (s.name.isEmpty()) s.name = QObject::tr("Premiere Sequence");
        bool fpsSet = false;
        for (QDomElement tg = PrReader::find(sq, "TrackGroups").firstChildElement("TrackGroup"); !tg.isNull(); tg = tg.nextSiblingElement("TrackGroup")) {
            QDomElement group = r.ref(tg.firstChildElement("Second"));
            if (group.isNull()) continue;
            const bool video = group.tagName().startsWith("Video");
            const TrackType type = video ? TrackType::Video : TrackType::Audio;
            QDomElement inner = group.firstChildElement("TrackGroup");
            if (!fpsSet) {
                const qint64 frameTicks = inner.firstChildElement("FrameRate").text().toLongLong();
                if (frameTicks > 0) {
                    const double fps = double(kTicksPerSecond) / double(frameTicks);
                    s.fps = FrameRate::fromDouble(fps);
                    fpsSet = true;
                }
            }
            if (video) {
                const int w = PrReader::find(group, "FrameRect").text().section(',', 2, 2).toInt();
                const int h = PrReader::find(group, "FrameRect").text().section(',', 3, 3).toInt();
                if (w > 0 && h > 0) { s.width = w & ~1; s.height = h & ~1; }
            }
            int ti = 0;
            for (QDomElement tr = inner.firstChildElement("Tracks").firstChildElement("Track"); !tr.isNull(); tr = tr.nextSiblingElement("Track"), ++ti) {
                QDomElement track = r.ref(tr);
                if (track.isNull()) continue;
                while ((int)s.tracks(type).size() <= ti) s.addTrack(type);
                QDomElement items = PrReader::find(track, "TrackItems");
                for (QDomElement ti2 = items.firstChildElement("TrackItem"); !ti2.isNull(); ti2 = ti2.nextSiblingElement("TrackItem")) {
                    QDomElement item = r.ref(ti2);
                    if (item.isNull() || !item.tagName().endsWith("ClipTrackItem")) continue;
                    QDomElement trackItem = PrReader::find(item, "TrackItem");
                    const qint64 start = trackItem.firstChildElement("Start").text().toLongLong();
                    const qint64 end = trackItem.firstChildElement("End").text().toLongLong();
                    if (end <= start) continue;
                    QDomElement sub = r.follow(item, "SubClip");
                    QDomElement clipObj = r.follow(sub, "Clip");
                    QDomElement inner2 = clipObj.firstChildElement("Clip");
                    const qint64 inPt = inner2.firstChildElement("InPoint").text().toLongLong();
                    const double speed = inner2.firstChildElement("PlaybackSpeed").text().isEmpty() ? 1.0 : inner2.firstChildElement("PlaybackSpeed").text().toDouble();
                    QDomElement source = r.follow(inner2, "Source");
                    QDomElement media = r.follow(source, "Media");
                    const QString mp = mediaPath(media);
                    if (mp.isEmpty()) continue;
                    MediaItem *m = mediaFor(project, mp, PrReader::find(sub, "Name").text());
                    Clip c;
                    c.id = newId();
                    c.source = ClipSource::Media;
                    c.mediaId = m->id;
                    c.name = PrReader::find(sub, "Name").text();
                    if (c.name.isEmpty()) c.name = m->name;
                    c.start = floorToFrame(start, s.fps);
                    c.duration = floorToFrame(end - start, s.fps);
                    c.in = inPt;
                    c.speed = speed > 0 ? speed : 1.0;
                    c.reverse = speed < 0;
                    s.tracks(type)[ti].clips.push_back(c);
                }
            }
        }
        // Link video and audio clips of the same media that start together.
        for (auto &vt : s.videoTracks)
            for (auto &vc : vt.clips)
                for (auto &at : s.audioTracks)
                    for (auto &ac : at.clips)
                        if (ac.mediaId == vc.mediaId && ac.start == vc.start && ac.linkId.isEmpty() && ac.duration == vc.duration) {
                            if (vc.linkId.isEmpty()) vc.linkId = newId();
                            ac.linkId = vc.linkId;
                        }
        finishSequence(s);
        if (s.duration() == 0) continue;
        project.sequences.push_back(s);
        created << s.id;
    }
    if (created.isEmpty() && error)
        *error = QObject::tr("No sequence could be read from this Premiere Pro project. Export it from Premiere with File > Export > Final Cut Pro XML for a complete transfer.");
    return created;
}

// ---------------------------------------------------------------------------

QStringList importAny(const QString &path, Project &project, FrameRate fpsHint, QString *error)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == "xml" || ext == "xmeml") return importFcp7Xml(path, project, error);
    if (ext == "edl") return importEdl(path, project, fpsHint, error);
    if (ext == "otio") return importOtio(path, project, error);
    if (ext == "prproj") return importPrproj(path, project, error);
    if (error) *error = QObject::tr("Unsupported format: %1").arg(ext);
    return {};
}

bool exportAny(const Project &project, const Sequence &seq, const QString &path, QString *error)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == "xml") return exportFcp7Xml(project, seq, path, error);
    if (ext == "edl") return exportEdl(project, seq, path, error);
    if (ext == "otio") return exportOtio(project, seq, path, error);
    if (ext == "fcpxml") return exportFcpxml(project, seq, path, error);
    if (error) *error = QObject::tr("Unsupported format: %1").arg(ext);
    return false;
}

} // namespace rc::interop
