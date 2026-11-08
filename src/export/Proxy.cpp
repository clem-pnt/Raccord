#include "Proxy.h"
#include "Encoder.h"
#include "ai/Analysis.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace rc {

QString proxyPathFor(const QString &cacheDir, const MediaItem &m, int height)
{
    const QByteArray h = QCryptographicHash::hash((m.path + QString::number(height)).toUtf8(), QCryptographicHash::Sha1).toHex().left(12);
    QDir().mkpath(cacheDir + "/Proxies");
    return cacheDir + "/Proxies/" + QFileInfo(m.path).completeBaseName() + "_" + h + "_proxy.mp4";
}

bool makeProxy(const MediaItem &src, const QString &dst, int height, const std::function<bool(double)> &progress, QString *error)
{
    if (!src.info.hasVideo || src.info.isStill) {
        if (error) *error = QObject::tr("Proxies are only made for video files");
        return false;
    }
    const bool rot = src.info.rotation % 180 != 0;
    const int sw = rot ? src.info.height : src.info.width, sh = rot ? src.info.width : src.info.height;
    const int h = std::min(height, sh) & ~1;
    const int w = std::max(2, int(std::lround(double(sw) * src.info.pixelAspect * h / std::max(1, sh)))) & ~1;
    ExportSettings s;
    s.path = QFileInfo(dst).path() + "/" + QFileInfo(dst).completeBaseName() + ".part.mp4";
    s.container = "mp4";
    s.videoCodec = "h264";
    s.audioCodec = "none";
    s.quality = 23;
    s.encoderPreset = "veryfast";
    s.keyframeInterval = 6;
    s.fastStart = false;
    const FrameRate fps = src.fps();
    Encoder enc;
    if (!enc.open(s, w, h, fps, 48000, 2, error))
        return false;
    // Proxies are written in display orientation (the decoder reports no rotation for them).
    const int rotation = ((src.info.rotation % 360) + 360) % 360;
    const int iw = rot ? h : w, ih = rot ? w : h;   // stored orientation of the source, scaled
    std::vector<uint16_t> rgba(size_t(w) * h * 4);
    int64_t last = -1;
    bool ok = true;
    const Ticks dur = std::max<Ticks>(1, src.info.duration);
    ai::scanFrames(src.path, 0, src.info.duration + kTicksPerSecond, iw, ih, false, [&](Ticks t, const uint8_t *d, int stride) {
        const int64_t idx = (int64_t)std::llround(double(t) / double(fps.frameTicks()));
        if (idx <= last) return true;
        for (int y = 0; y < h; ++y) {
            uint16_t *o = &rgba[size_t(y) * w * 4];
            for (int x = 0; x < w; ++x) {
                int sx = x, sy = y;
                if (rotation == 90) { sx = y; sy = ih - 1 - x; }
                else if (rotation == 270) { sx = iw - 1 - y; sy = x; }
                else if (rotation == 180) { sx = iw - 1 - x; sy = ih - 1 - y; }
                const uint8_t *px = d + sy * stride + sx * 3;
                o[x * 4 + 0] = uint16_t(px[0] * 257);
                o[x * 4 + 1] = uint16_t(px[1] * 257);
                o[x * 4 + 2] = uint16_t(px[2] * 257);
                o[x * 4 + 3] = 65535;
            }
        }
        // Repeat frames to fill gaps (variable frame rate sources).
        for (int64_t f = last + 1; f <= idx; ++f)
            if (!enc.writeVideo(rgba.data(), f)) { ok = false; return false; }
        last = idx;
        return !progress || progress(double(t) / dur);
    }, error);
    if (!ok || !enc.finish(error)) {
        QFile::remove(s.path);
        return false;
    }
    QFile::remove(dst);
    if (!QFile::rename(s.path, dst)) {
        if (error) *error = QObject::tr("Cannot write %1").arg(dst);
        return false;
    }
    return true;
}

} // namespace rc
