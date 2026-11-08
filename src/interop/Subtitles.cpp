#include "Subtitles.h"
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>

namespace rc {

static QString stamp(Ticks t, bool vtt)
{
    if (t < 0) t = 0;
    int64_t ms = t / (kTicksPerSecond / 1000);
    int64_t h = ms / 3600000, m = (ms / 60000) % 60, s = (ms / 1000) % 60, r = ms % 1000;
    return QString("%1:%2:%3%4%5").arg(h, 2, 10, QChar('0')).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'))
        .arg(vtt ? "." : ",").arg(r, 3, 10, QChar('0'));
}

QString toSrt(const CaptionTrack &track, Ticks offset)
{
    QString out;
    int n = 1;
    for (const auto &c : track.captions) {
        if (c.end - offset <= 0)
            continue;
        out += QString::number(n++) + "\n" + stamp(c.start - offset, false) + " --> " + stamp(c.end - offset, false) + "\n" + c.text.trimmed() + "\n\n";
    }
    return out;
}

static bool writeText(const QString &path, const QString &text)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    f.write(text.toUtf8());
    return true;
}

bool writeSrt(const CaptionTrack &track, const QString &path, Ticks offset) { return writeText(path, toSrt(track, offset)); }

bool writeVtt(const CaptionTrack &track, const QString &path, Ticks offset)
{
    QString out = "WEBVTT\n\n";
    for (const auto &c : track.captions)
        if (c.end - offset > 0)
            out += stamp(c.start - offset, true) + " --> " + stamp(c.end - offset, true) + "\n" + c.text.trimmed() + "\n\n";
    return writeText(path, out);
}

static Ticks parseStamp(const QString &s)
{
    QRegularExpression re("(?:(\\d+):)?(\\d+):(\\d+)[,.](\\d+)");
    auto m = re.match(s.trimmed());
    if (!m.hasMatch())
        return kInvalidTicks;
    int64_t h = m.captured(1).toLongLong(), mi = m.captured(2).toLongLong(), se = m.captured(3).toLongLong();
    QString frac = m.captured(4).leftJustified(3, '0').left(3);
    int64_t ms = ((h * 60 + mi) * 60 + se) * 1000 + frac.toLongLong();
    return ms * (kTicksPerSecond / 1000);
}

CaptionTrack parseSrt(const QString &textIn)
{
    CaptionTrack t;
    t.id = newId();
    QString text = textIn;
    text.replace("\r\n", "\n");
    const QStringList blocks = text.split(QRegularExpression("\n\\s*\n"), Qt::SkipEmptyParts);
    for (const auto &b : blocks) {
        QStringList lines = b.split('\n');
        int i = 0;
        while (i < lines.size() && !lines[i].contains("-->"))
            ++i;
        if (i >= lines.size())
            continue;
        QStringList parts = lines[i].split("-->");
        if (parts.size() != 2)
            continue;
        Caption c;
        c.id = newId();
        c.start = parseStamp(parts[0]);
        c.end = parseStamp(parts[1].section(' ', 0, 1, QString::SectionSkipEmpty));
        if (c.start == kInvalidTicks || c.end == kInvalidTicks)
            continue;
        c.text = lines.mid(i + 1).join('\n').remove(QRegularExpression("<[^>]+>")).trimmed();
        t.captions.push_back(c);
    }
    t.sort();
    return t;
}

bool readSubtitles(const QString &path, CaptionTrack &out, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = f.errorString();
        return false;
    }
    QString text = QString::fromUtf8(f.readAll());
    out = parseSrt(text.startsWith("WEBVTT") ? text.mid(6) : text);
    out.name = QFileInfo(path).completeBaseName();
    if (out.captions.empty()) {
        if (error) *error = "No captions found";
        return false;
    }
    return true;
}

void reflowCaptions(CaptionTrack &track)
{
    const int maxChars = std::max(10, track.style.maxCharsPerLine * std::max(1, track.style.maxLines));
    std::vector<Caption> out;
    for (const auto &c : track.captions) {
        QStringList words = c.text.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (c.text.size() <= maxChars || words.size() < 2) {
            out.push_back(c);
            continue;
        }
        QList<QStringList> chunks;
        QStringList cur;
        int len = 0;
        for (const auto &w : words) {
            if (len + w.size() + 1 > maxChars && !cur.isEmpty()) {
                chunks << cur;
                cur.clear();
                len = 0;
            }
            cur << w;
            len += w.size() + 1;
        }
        if (!cur.isEmpty()) chunks << cur;
        const Ticks dur = c.end - c.start;
        const int totalChars = c.text.size();
        Ticks pos = c.start;
        for (int i = 0; i < chunks.size(); ++i) {
            QString t = chunks[i].join(' ');
            Caption n = c;
            n.id = newId();
            n.text = t;
            n.start = pos;
            n.end = i + 1 == chunks.size() ? c.end : pos + dur * t.size() / std::max(1, totalChars);
            pos = n.end;
            out.push_back(n);
        }
    }
    track.captions = out;
}

} // namespace rc
