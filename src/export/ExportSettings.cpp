#include "ExportSettings.h"
#include "media/FFmpeg.h"
#include <QJsonArray>

namespace rc {

QJsonObject ExportSettings::toJson() const
{
    QJsonObject o;
    o["path"] = path;
    o["container"] = container;
    o["videoCodec"] = videoCodec;
    o["width"] = width;
    o["height"] = height;
    o["fps"] = QJsonArray{fps.num, fps.den};
    o["rateControl"] = rateControl;
    o["quality"] = quality;
    o["videoBitrate"] = videoBitrateKbps;
    o["preset"] = encoderPreset;
    o["profile"] = profile;
    o["tenBit"] = tenBit;
    o["alpha"] = alpha;
    o["audioCodec"] = audioCodec;
    o["audioBitrate"] = audioBitrateKbps;
    o["sampleRate"] = sampleRate;
    o["channels"] = channels;
    o["useWorkArea"] = useWorkArea;
    o["useProxies"] = useProxies;
    o["loudness"] = loudnessTarget;
    o["truePeak"] = truePeakDb;
    o["burnCaptions"] = burnCaptions;
    o["sidecarSrt"] = sidecarSrt;
    o["keyint"] = keyframeInterval;
    return o;
}

ExportSettings ExportSettings::fromJson(const QJsonObject &o)
{
    ExportSettings s;
    s.path = o["path"].toString();
    s.container = o["container"].toString("mp4");
    s.videoCodec = o["videoCodec"].toString("h264");
    s.width = o["width"].toInt();
    s.height = o["height"].toInt();
    QJsonArray f = o["fps"].toArray();
    if (f.size() == 2) s.fps = {f[0].toInt(), f[1].toInt()};
    s.rateControl = o["rateControl"].toString("crf");
    s.quality = o["quality"].toInt(20);
    s.videoBitrateKbps = o["videoBitrate"].toInt(16000);
    s.encoderPreset = o["preset"].toString("medium");
    s.profile = o["profile"].toString();
    s.tenBit = o["tenBit"].toBool();
    s.alpha = o["alpha"].toBool();
    s.audioCodec = o["audioCodec"].toString("aac");
    s.audioBitrateKbps = o["audioBitrate"].toInt(320);
    s.sampleRate = o["sampleRate"].toInt();
    s.channels = o["channels"].toInt(2);
    s.useWorkArea = o["useWorkArea"].toBool(true);
    s.useProxies = o["useProxies"].toBool();
    s.loudnessTarget = o["loudness"].toDouble();
    s.truePeakDb = o["truePeak"].toDouble(-1);
    s.burnCaptions = o["burnCaptions"].toBool();
    s.sidecarSrt = o["sidecarSrt"].toBool();
    s.keyframeInterval = o["keyint"].toInt();
    return s;
}

QString ExportSettings::extension() const
{
    if (container == "png" || container == "tiff" || container == "jpg")
        return container;
    return container;
}

static ExportSettings mk(const QString &container, const QString &v, const QString &a, int q = 20, int w = 0, int h = 0)
{
    ExportSettings s;
    s.container = container;
    s.videoCodec = v;
    s.audioCodec = a;
    s.quality = q;
    s.width = w;
    s.height = h;
    return s;
}

const std::vector<ExportPreset> &exportPresets()
{
    static std::vector<ExportPreset> p = [] {
        std::vector<ExportPreset> v;
        auto add = [&](const QString &id, const QString &name, const QString &cat, const QString &desc, ExportSettings s) {
            v.push_back({id, name, cat, desc, s});
        };
        ExportSettings yt = mk("mp4", "h264", "aac", 18);
        yt.audioBitrateKbps = 320;
        yt.encoderPreset = "slow";
        yt.loudnessTarget = -14;
        add("youtube-1080", "YouTube 1080p (H.264)", "Web", "H.264 high quality, AAC 320 kbps, -14 LUFS", [&] { auto s = yt; s.width = 1920; s.height = 1080; return s; }());
        add("youtube-4k", "YouTube 4K (H.264)", "Web", "H.264 2160p, AAC 320 kbps", [&] { auto s = yt; s.width = 3840; s.height = 2160; s.quality = 19; return s; }());
        add("youtube-4k-hevc", "YouTube 4K HDR-ready (HEVC 10-bit)", "Web", "HEVC Main10", [&] { auto s = mk("mp4", "hevc", "aac", 20, 3840, 2160); s.tenBit = true; return s; }());
        add("match-source", "Match Sequence (H.264 high bitrate)", "Master", "Sequence size and frame rate", mk("mp4", "h264", "aac", 16));
        add("tiktok", "TikTok / Reels / Shorts (9:16 1080x1920)", "Social", "Vertical 1080x1920, H.264", [&] { auto s = mk("mp4", "h264", "aac", 19, 1080, 1920); s.loudnessTarget = -14; return s; }());
        add("instagram-square", "Instagram Feed (1:1 1080x1080)", "Social", "Square H.264", mk("mp4", "h264", "aac", 19, 1080, 1080));
        add("instagram-portrait", "Instagram Portrait (4:5 1080x1350)", "Social", "Portrait H.264", mk("mp4", "h264", "aac", 19, 1080, 1350));
        add("twitter", "X / Twitter (720p)", "Social", "H.264 720p", mk("mp4", "h264", "aac", 21, 1280, 720));
        add("vimeo", "Vimeo 1080p", "Web", "H.264", mk("mp4", "h264", "aac", 17, 1920, 1080));
        add("web-small", "Small file (H.264 720p)", "Web", "Quick review copy", [&] { auto s = mk("mp4", "h264", "aac", 26, 1280, 720); s.encoderPreset = "faster"; s.audioBitrateKbps = 128; return s; }());
        add("hevc-1080", "HEVC 1080p (smaller files)", "Web", "H.265", mk("mp4", "hevc", "aac", 22, 1920, 1080));
        add("av1-1080", "AV1 1080p (best compression)", "Web", "SVT-AV1", [&] { auto s = mk("mp4", "av1", "opus", 30, 1920, 1080); s.encoderPreset = "8"; return s; }());
        add("webm-vp9", "WebM VP9", "Web", "VP9 + Opus", mk("webm", "vp9", "opus", 31));
        add("prores-422", "Apple ProRes 422", "Master", "ProRes 422 10-bit, PCM 24-bit", [&] { auto s = mk("mov", "prores", "pcm24"); s.profile = "standard"; s.tenBit = true; return s; }());
        add("prores-422-hq", "Apple ProRes 422 HQ", "Master", "ProRes 422 HQ 10-bit", [&] { auto s = mk("mov", "prores", "pcm24"); s.profile = "hq"; s.tenBit = true; return s; }());
        add("prores-proxy", "Apple ProRes 422 Proxy", "Master", "Lightweight intermediate", [&] { auto s = mk("mov", "prores", "pcm24"); s.profile = "proxy"; s.tenBit = true; return s; }());
        add("prores-4444", "Apple ProRes 4444 (with alpha)", "Master", "ProRes 4444 + alpha channel", [&] { auto s = mk("mov", "prores", "pcm24"); s.profile = "4444"; s.tenBit = true; s.alpha = true; return s; }());
        add("dnxhr-hq", "Avid DNxHR HQ", "Master", "DNxHR HQ (8-bit 4:2:2) in MOV", [&] { auto s = mk("mov", "dnxhr", "pcm24"); s.profile = "hq"; return s; }());
        add("dnxhr-hqx", "Avid DNxHR HQX 10-bit", "Master", "DNxHR HQX (10-bit 4:2:2) in MXF", [&] { auto s = mk("mxf", "dnxhr", "pcm24"); s.profile = "hqx"; s.tenBit = true; return s; }());
        add("broadcast-h264", "Broadcast H.264 CBR 50 Mb/s", "Broadcast", "CBR 50 Mb/s, -23 LUFS (EBU R128)", [&] { auto s = mk("mp4", "h264", "pcm24"); s.container = "mov"; s.rateControl = "cbr"; s.videoBitrateKbps = 50000; s.loudnessTarget = -23; return s; }());
        add("nvenc-h264", "H.264 NVIDIA GPU (NVENC)", "Hardware", "Fast export on NVIDIA", mk("mp4", "h264_nvenc", "aac", 21));
        add("nvenc-hevc", "HEVC NVIDIA GPU (NVENC)", "Hardware", "Fast export on NVIDIA", mk("mp4", "hevc_nvenc", "aac", 23));
        add("png-sequence", "PNG Sequence", "Image", "Lossless frames with alpha", [&] { auto s = mk("png", "png", "none"); s.alpha = true; return s; }());
        add("tiff-sequence", "TIFF Sequence", "Image", "16-bit TIFF frames", mk("tiff", "tiff", "none"));
        add("jpg-sequence", "JPEG Sequence", "Image", "Frames for review", mk("jpg", "mjpeg", "none", 3));
        add("wav", "Audio WAV 24-bit", "Audio", "Uncompressed audio", mk("wav", "none", "pcm24"));
        add("mp3", "Audio MP3 320 kbps", "Audio", "MP3", mk("mp3", "none", "mp3"));
        add("flac", "Audio FLAC", "Audio", "Lossless compressed", mk("flac", "none", "flac"));
        add("podcast", "Podcast (AAC, -16 LUFS)", "Audio", "AAC 192 kbps mono-compatible, -16 LUFS", [&] { auto s = mk("m4a", "none", "aac"); s.audioBitrateKbps = 192; s.loudnessTarget = -16; return s; }());
        return v;
    }();
    return p;
}

const ExportPreset *findPreset(const QString &id)
{
    for (const auto &p : exportPresets())
        if (p.id == id)
            return &p;
    return nullptr;
}

bool encoderAvailable(const QString &videoCodec)
{
    const char *name = nullptr;
    if (videoCodec == "h264") name = "libx264";
    else if (videoCodec == "hevc") name = "libx265";
    else if (videoCodec == "prores") name = "prores_ks";
    else if (videoCodec == "dnxhr") name = "dnxhd";
    else if (videoCodec == "vp9") name = "libvpx-vp9";
    else if (videoCodec == "av1") name = "libsvtav1";
    else if (videoCodec == "png") name = "png";
    else if (videoCodec == "tiff") name = "tiff";
    else if (videoCodec == "mjpeg") name = "mjpeg";
    else if (videoCodec == "none") return true;
    else name = nullptr;
    QByteArray n = name ? QByteArray(name) : videoCodec.toUtf8();
    const AVCodec *c = avcodec_find_encoder_by_name(n.constData());
    if (!c)
        return false;
    if (videoCodec.endsWith("_nvenc")) {
        // Probe that a CUDA device actually exists.
        return ff::hwDevice(AV_HWDEVICE_TYPE_CUDA) != nullptr;
    }
    return true;
}

} // namespace rc
