#include "MediaProbe.h"
#include "FFmpeg.h"
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

namespace rc {

QStringList importFilters()
{
    return {"*.mp4", "*.mov", "*.mkv", "*.avi", "*.mxf", "*.webm", "*.m4v", "*.mts", "*.m2ts", "*.ts", "*.mpg",
            "*.mpeg", "*.wmv", "*.flv", "*.3gp", "*.dv", "*.braw", "*.r3d", "*.y4m", "*.gif",
            "*.wav", "*.mp3", "*.aac", "*.m4a", "*.flac", "*.ogg", "*.opus", "*.aif", "*.aiff", "*.wma", "*.ac3",
            "*.png", "*.jpg", "*.jpeg", "*.tif", "*.tiff", "*.bmp", "*.webp", "*.exr", "*.dpx", "*.tga", "*.psd",
            "*.heic", "*.avif", "*.jxl", "*.svg"};
}

bool isImportable(const QString &path)
{
    const QString ext = "*." + QFileInfo(path).suffix().toLower();
    return importFilters().contains(ext);
}

static bool isImageExt(const QString &suffix)
{
    static const QStringList img = {"png", "jpg", "jpeg", "tif", "tiff", "bmp", "webp", "exr", "dpx", "tga", "psd", "heic", "avif", "jxl"};
    return img.contains(suffix.toLower());
}

QString detectImageSequence(const QString &path, int *firstNumber, int *count)
{
    QFileInfo fi(path);
    if (!isImageExt(fi.suffix()))
        return {};
    QRegularExpression re("^(.*?)(\\d+)$");
    auto m = re.match(fi.completeBaseName());
    if (!m.hasMatch())
        return {};
    const QString prefix = m.captured(1);
    const QString digits = m.captured(2);
    const int width = digits.size();
    QDir dir = fi.absoluteDir();
    QStringList files = dir.entryList({prefix + QString("?").repeated(width) + "." + fi.suffix()}, QDir::Files, QDir::Name);
    if (files.size() < 2)
        return {};
    int first = INT32_MAX;
    for (const auto &f : files) {
        auto mm = re.match(QFileInfo(f).completeBaseName());
        if (mm.hasMatch())
            first = std::min(first, mm.captured(2).toInt());
    }
    if (firstNumber) *firstNumber = first;
    if (count) *count = files.size();
    return dir.absoluteFilePath(prefix + QString("%0%1d").arg(width) + "." + fi.suffix());
}

static QString tagValue(AVDictionary *d, const char *key)
{
    AVDictionaryEntry *e = av_dict_get(d, key, nullptr, 0);
    return e ? QString::fromUtf8(e->value) : QString();
}

MediaInfo probeMedia(const QString &path)
{
    MediaInfo info;
    QString err;
    ff::FormatCtx fmt = ff::openInput(path, &err);
    if (!fmt) {
        info.error = err.isEmpty() ? QStringLiteral("Cannot open file") : err;
        return info;
    }
    AVFormatContext *ctx = fmt.get();
    info.container = QString::fromUtf8(ctx->iformat->name);
    info.bitrate = ctx->bit_rate;
    if (ctx->duration != AV_NOPTS_VALUE)
        info.duration = rationalToTicks(ctx->duration, 1, AV_TIME_BASE);
    if (ctx->start_time != AV_NOPTS_VALUE)
        info.startTime = rationalToTicks(ctx->start_time, 1, AV_TIME_BASE);
    info.timecode = tagValue(ctx->metadata, "timecode");

    const QString suffix = QFileInfo(path).suffix().toLower();
    const bool imagePipe = info.container.contains("_pipe") || info.container == "image2" || isImageExt(suffix);

    int vIdx = av_find_best_stream(ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vIdx >= 0) {
        AVStream *st = ctx->streams[vIdx];
        AVCodecParameters *par = st->codecpar;
        if (!(st->disposition & AV_DISPOSITION_ATTACHED_PIC) || imagePipe) {
            info.hasVideo = true;
            info.width = par->width;
            info.height = par->height;
            if (par->sample_aspect_ratio.num > 0 && par->sample_aspect_ratio.den > 0)
                info.pixelAspect = av_q2d(par->sample_aspect_ratio);
            const AVCodecDescriptor *cd = avcodec_descriptor_get(par->codec_id);
            info.videoCodec = cd ? QString::fromUtf8(cd->name) : QString();
            const AVPixFmtDescriptor *pd = av_pix_fmt_desc_get(AVPixelFormat(par->format));
            if (pd) {
                info.pixelFormat = QString::fromUtf8(pd->name);
                info.bitDepth = pd->comp[0].depth;
                info.hasAlpha = (pd->flags & AV_PIX_FMT_FLAG_ALPHA) != 0;
            }
            AVRational fr = st->avg_frame_rate.num > 0 ? st->avg_frame_rate : st->r_frame_rate;
            if (fr.num > 0 && fr.den > 0) {
                info.fps = FrameRate::fromDouble(av_q2d(fr));
                if (st->r_frame_rate.num > 0 && st->avg_frame_rate.num > 0 &&
                    std::abs(av_q2d(st->r_frame_rate) - av_q2d(st->avg_frame_rate)) > 0.5)
                    info.variableFrameRate = true;
            }
            info.colorPrimaries = QString::fromUtf8(av_color_primaries_name(par->color_primaries));
            info.colorTransfer = QString::fromUtf8(av_color_transfer_name(par->color_trc));
            info.colorSpace = QString::fromUtf8(av_color_space_name(par->color_space));
            info.fullRange = par->color_range == AVCOL_RANGE_JPEG;
            info.hdr = par->color_trc == AVCOL_TRC_SMPTE2084 || par->color_trc == AVCOL_TRC_ARIB_STD_B67;
            info.interlaced = par->field_order != AV_FIELD_PROGRESSIVE && par->field_order != AV_FIELD_UNKNOWN;
            // Rotation from the display matrix side data.
            const uint8_t *sd = nullptr;
#if LIBAVFORMAT_VERSION_MAJOR >= 61
            const AVPacketSideData *psd = av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
            sd = psd ? psd->data : nullptr;
#else
            sd = av_stream_get_side_data(st, AV_PKT_DATA_DISPLAYMATRIX, nullptr);
#endif
            if (sd) {
                double rot = av_display_rotation_get((const int32_t *)sd);
                info.rotation = ((int)std::lround(-rot) % 360 + 360) % 360;
            }
            if (info.timecode.isEmpty())
                info.timecode = tagValue(st->metadata, "timecode");
            if (st->duration != AV_NOPTS_VALUE && st->duration > 0) {
                Ticks sd2 = rationalToTicks(st->duration, st->time_base.num, st->time_base.den);
                if (sd2 > 0)
                    info.duration = sd2;
            }
            if (st->start_time != AV_NOPTS_VALUE)
                info.startTime = rationalToTicks(st->start_time, st->time_base.num, st->time_base.den);
            const bool single = imagePipe && !path.contains('%') && (st->nb_frames <= 1);
            if (single || (par->codec_id == AV_CODEC_ID_PNG && info.container != "apng" && st->nb_frames <= 1 && imagePipe)) {
                info.isStill = true;
                info.duration = 0;
            }
            if (path.contains('%')) {
                info.isImageSequence = true;
                int64_t frames = st->nb_frames > 0 ? st->nb_frames : 0;
                if (frames <= 0 && st->duration > 0)
                    frames = st->duration;
                if (frames > 0)
                    info.duration = frameToTicks(frames, info.fps);
            }
        }
    }
    int aIdx = av_find_best_stream(ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (aIdx >= 0) {
        AVStream *st = ctx->streams[aIdx];
        AVCodecParameters *par = st->codecpar;
        info.hasAudio = true;
        info.sampleRate = par->sample_rate;
#if LIBAVUTIL_VERSION_MAJOR >= 57 && LIBAVUTIL_VERSION_MINOR >= 24
        info.channels = par->ch_layout.nb_channels;
#else
        info.channels = par->channels;
#endif
        const AVCodecDescriptor *cd = avcodec_descriptor_get(par->codec_id);
        info.audioCodec = cd ? QString::fromUtf8(cd->name) : QString();
        for (unsigned i = 0; i < ctx->nb_streams; ++i)
            if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
                ++info.audioStreams;
        if (!info.hasVideo && st->duration != AV_NOPTS_VALUE && st->duration > 0)
            info.duration = rationalToTicks(st->duration, st->time_base.num, st->time_base.den);
    }
    info.valid = info.hasVideo || info.hasAudio;
    if (!info.valid)
        info.error = QStringLiteral("No audio or video stream");
    if (info.hasVideo && !info.isStill && info.duration <= 0)
        info.duration = frameToTicks(1, info.fps);
    return info;
}

} // namespace rc
