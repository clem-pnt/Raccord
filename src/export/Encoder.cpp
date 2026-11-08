#include "Encoder.h"
extern "C" {
#include <libavutil/audio_fifo.h>
}
#include <QFileInfo>
#include <QDir>

namespace rc {

Encoder::~Encoder()
{
    if (m_vctx) avcodec_free_context(&m_vctx);
    if (m_actx) avcodec_free_context(&m_actx);
    if (m_sws) sws_freeContext(m_sws);
    if (m_swr) swr_free(&m_swr);
    if (m_fifo) av_audio_fifo_free(m_fifo);
    if (m_vframe) av_frame_free(&m_vframe);
    if (m_out) {
        if (!(m_out->oformat->flags & AVFMT_NOFILE) && m_out->pb)
            avio_closep(&m_out->pb);
        avformat_free_context(m_out);
    }
}

static const char *encoderName(const QString &v)
{
    if (v == "h264") return "libx264";
    if (v == "hevc") return "libx265";
    if (v == "prores") return "prores_ks";
    if (v == "dnxhr") return "dnxhd";
    if (v == "vp9") return "libvpx-vp9";
    if (v == "av1") return "libsvtav1";
    if (v == "png") return "png";
    if (v == "tiff") return "tiff";
    if (v == "mjpeg") return "mjpeg";
    if (v == "h264_nvenc") return "h264_nvenc";
    if (v == "hevc_nvenc") return "hevc_nvenc";
    return nullptr;
}

bool Encoder::open(const ExportSettings &s, int width, int height, FrameRate fps, int sampleRate, int channels, QString *error)
{
    ff::initLogging();
    m_w = width & ~1;
    m_h = height & ~1;
    m_fps = fps;
    m_rate = sampleRate;
    m_channels = channels;
    m_imageSeq = s.isImageSequence();
    QString path = s.path;
    const char *fmtName = nullptr;
    if (m_imageSeq) {
        // name_%05d.png
        QFileInfo fi(path);
        QString base = fi.completeBaseName();
        if (!base.contains('%'))
            base += "_%05d";
        path = fi.absolutePath() + "/" + base + "." + s.container;
        fmtName = "image2";
        m_w = width;
        m_h = height;
    } else if (s.container == "m4a") {
        fmtName = "ipod";
    } else if (s.container == "mxf") {
        fmtName = "mxf";
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QByteArray p = path.toUtf8();
    int r = avformat_alloc_output_context2(&m_out, nullptr, fmtName, p.constData());
    if (r < 0 || !m_out) {
        if (error) *error = "Cannot create output: " + ff::errorString(r);
        return false;
    }
    if (!s.isAudioOnly() && !openVideo(s, error))
        return false;
    if (!m_imageSeq && s.audioCodec != "none" && !openAudio(s, error))
        return false;
    if (!(m_out->oformat->flags & AVFMT_NOFILE)) {
        r = avio_open(&m_out->pb, p.constData(), AVIO_FLAG_WRITE);
        if (r < 0) {
            if (error) *error = "Cannot open file for writing: " + ff::errorString(r);
            return false;
        }
    }
    AVDictionary *opts = nullptr;
    if (s.fastStart && (s.container == "mp4" || s.container == "mov" || s.container == "m4a"))
        av_dict_set(&opts, "movflags", "+faststart", 0);
    if (!s.title.isEmpty())
        av_dict_set(&m_out->metadata, "title", s.title.toUtf8().constData(), 0);
    av_dict_set(&m_out->metadata, "encoder", "Raccord", 0);
    r = avformat_write_header(m_out, &opts);
    av_dict_free(&opts);
    if (r < 0) {
        if (error) *error = "Cannot write header: " + ff::errorString(r);
        return false;
    }
    return true;
}

bool Encoder::openVideo(const ExportSettings &s, QString *error)
{
    const char *name = encoderName(s.videoCodec);
    const AVCodec *codec = name ? avcodec_find_encoder_by_name(name) : nullptr;
    if (!codec) {
        if (error) *error = QString("Video encoder not available: %1").arg(s.videoCodec);
        return false;
    }
    m_vctx = avcodec_alloc_context3(codec);
    m_vctx->width = m_w;
    m_vctx->height = m_h;
    m_vctx->time_base = AVRational{m_fps.den, m_fps.num};
    m_vctx->framerate = AVRational{m_fps.num, m_fps.den};
    m_vctx->sample_aspect_ratio = AVRational{1, 1};
    m_vctx->gop_size = s.keyframeInterval > 0 ? s.keyframeInterval : std::max(1, m_fps.nominal() * 2);
    m_vctx->color_primaries = AVCOL_PRI_BT709;
    m_vctx->color_trc = AVCOL_TRC_BT709;
    m_vctx->colorspace = AVCOL_SPC_BT709;
    m_vctx->color_range = AVCOL_RANGE_MPEG;
    m_vctx->thread_count = 0;
    AVPixelFormat pix = AV_PIX_FMT_YUV420P;
    AVDictionary *o = nullptr;
    const QString v = s.videoCodec;
    if (v == "h264") {
        pix = s.tenBit ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
        av_dict_set(&o, "preset", s.encoderPreset.toUtf8().constData(), 0);
        if (s.rateControl == "crf") av_dict_set_int(&o, "crf", s.quality, 0);
        av_dict_set(&o, "x264-params", "colorprim=bt709:transfer=bt709:colormatrix=bt709", 0);
        if (!s.tenBit) m_vctx->profile = FF_PROFILE_H264_HIGH;
    } else if (v == "hevc") {
        pix = s.tenBit ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
        av_dict_set(&o, "preset", s.encoderPreset.toUtf8().constData(), 0);
        if (s.rateControl == "crf") av_dict_set_int(&o, "crf", s.quality, 0);
        av_dict_set(&o, "tag", "hvc1", 0);
        av_dict_set(&o, "x265-params", "log-level=error:colorprim=bt709:transfer=bt709:colormatrix=bt709", 0);
        m_vctx->codec_tag = MKTAG('h', 'v', 'c', '1');
    } else if (v == "prores") {
        static const QStringList profiles = {"proxy", "lt", "standard", "hq", "4444", "4444xq"};
        int prof = std::max<int>(0, (int)profiles.indexOf(s.profile.isEmpty() ? QString("hq") : s.profile));
        av_dict_set_int(&o, "profile", prof, 0);
        av_dict_set(&o, "vendor", "apl0", 0);
        pix = prof >= 4 ? (s.alpha ? AV_PIX_FMT_YUVA444P10LE : AV_PIX_FMT_YUV444P10LE) : AV_PIX_FMT_YUV422P10LE;
        if (prof >= 4 && s.alpha) av_dict_set_int(&o, "alpha_bits", 16, 0);
    } else if (v == "dnxhr") {
        QString prof = s.profile.isEmpty() ? "hq" : s.profile;
        av_dict_set(&o, "profile", ("dnxhr_" + prof).toUtf8().constData(), 0);
        pix = (prof == "hqx") ? AV_PIX_FMT_YUV422P10LE : prof == "444" ? AV_PIX_FMT_YUV444P10LE : AV_PIX_FMT_YUV422P;
    } else if (v == "vp9") {
        pix = s.alpha ? AV_PIX_FMT_YUVA420P : AV_PIX_FMT_YUV420P;
        av_dict_set_int(&o, "crf", s.quality, 0);
        av_dict_set(&o, "b", "0", 0);
        av_dict_set(&o, "row-mt", "1", 0);
        av_dict_set(&o, "deadline", "good", 0);
        av_dict_set(&o, "cpu-used", "2", 0);
    } else if (v == "av1") {
        pix = s.tenBit ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
        av_dict_set_int(&o, "crf", s.quality, 0);
        av_dict_set(&o, "preset", s.encoderPreset.isEmpty() ? "8" : s.encoderPreset.toUtf8().constData(), 0);
    } else if (v == "png") {
        pix = s.alpha ? AV_PIX_FMT_RGBA : AV_PIX_FMT_RGB24;
        m_vctx->color_range = AVCOL_RANGE_JPEG;
    } else if (v == "tiff") {
        pix = s.alpha ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGB48LE;
        m_vctx->color_range = AVCOL_RANGE_JPEG;
    } else if (v == "mjpeg") {
        pix = AV_PIX_FMT_YUVJ420P;
        m_vctx->color_range = AVCOL_RANGE_JPEG;
        m_vctx->flags |= AV_CODEC_FLAG_QSCALE;
        m_vctx->global_quality = FF_QP2LAMBDA * std::clamp(s.quality, 2, 31);
    } else if (v.endsWith("_nvenc")) {
        pix = AV_PIX_FMT_YUV420P;
        av_dict_set(&o, "preset", "p5", 0);
        av_dict_set(&o, "rc", "vbr", 0);
        av_dict_set_int(&o, "cq", s.quality, 0);
    }
    if (s.rateControl == "vbr" || s.rateControl == "cbr") {
        m_vctx->bit_rate = int64_t(s.videoBitrateKbps) * 1000;
        if (s.rateControl == "cbr") {
            m_vctx->rc_max_rate = m_vctx->bit_rate;
            m_vctx->rc_min_rate = m_vctx->bit_rate;
            m_vctx->rc_buffer_size = int(m_vctx->bit_rate);
            if (v == "h264") av_dict_set(&o, "nal-hrd", "cbr", 0);
        } else {
            m_vctx->rc_max_rate = m_vctx->bit_rate * 3 / 2;
            m_vctx->rc_buffer_size = int(m_vctx->bit_rate * 2);
        }
    }
    m_vctx->pix_fmt = pix;
    if (m_out->oformat->flags & AVFMT_GLOBALHEADER)
        m_vctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    int r = avcodec_open2(m_vctx, codec, &o);
    av_dict_free(&o);
    if (r < 0) {
        if (error) *error = "Cannot open video encoder: " + ff::errorString(r);
        return false;
    }
    m_vst = avformat_new_stream(m_out, nullptr);
    m_vst->time_base = m_vctx->time_base;
    m_vst->avg_frame_rate = m_vctx->framerate;
    avcodec_parameters_from_context(m_vst->codecpar, m_vctx);
    if (v == "hevc")
        m_vst->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
    m_vframe = av_frame_alloc();
    m_vframe->format = pix;
    m_vframe->width = m_w;
    m_vframe->height = m_h;
    av_frame_get_buffer(m_vframe, 32);
    m_sws = sws_getContext(m_w, m_h, AV_PIX_FMT_RGBA64LE, m_w, m_h, pix, SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT, nullptr, nullptr, nullptr);
    if (!m_sws) {
        if (error) *error = "Cannot create colour converter";
        return false;
    }
    const int *coef = sws_getCoefficients(SWS_CS_ITU709);
    const bool fullOut = m_vctx->color_range == AVCOL_RANGE_JPEG;
    sws_setColorspaceDetails(m_sws, coef, 1, coef, fullOut ? 1 : 0, 0, 1 << 16, 1 << 16);
    return true;
}

bool Encoder::openAudio(const ExportSettings &s, QString *error)
{
    const QString a = s.audioCodec;
    const char *name = a == "aac" ? "aac" : a == "opus" ? "libopus" : a == "flac" ? "flac" : a == "pcm16" ? "pcm_s16le"
                     : a == "pcm24" ? "pcm_s24le" : a == "mp3" ? "libmp3lame" : nullptr;
    const AVCodec *codec = name ? avcodec_find_encoder_by_name(name) : nullptr;
    if (!codec) {
        if (error) *error = QString("Audio encoder not available: %1").arg(a);
        return false;
    }
    m_actx = avcodec_alloc_context3(codec);
    int rate = m_rate;
    if (a == "opus" && rate != 48000) rate = 48000;
    m_actx->sample_rate = rate;
    av_channel_layout_default(&m_actx->ch_layout, m_channels);
    // Pick a supported sample format.
    AVSampleFormat want = AV_SAMPLE_FMT_FLTP;
    if (codec->sample_fmts) {
        want = codec->sample_fmts[0];
        for (const AVSampleFormat *f = codec->sample_fmts; *f != AV_SAMPLE_FMT_NONE; ++f)
            if (*f == AV_SAMPLE_FMT_FLTP || *f == AV_SAMPLE_FMT_FLT || (a == "pcm24" && *f == AV_SAMPLE_FMT_S32)) {
                want = *f;
                break;
            }
    }
    m_actx->sample_fmt = want;
    if (a == "pcm24")
        m_actx->bits_per_raw_sample = 24;
    if (a == "flac")
        m_actx->sample_fmt = AV_SAMPLE_FMT_S32, m_actx->bits_per_raw_sample = 24;
    if (a != "flac" && !a.startsWith("pcm"))
        m_actx->bit_rate = int64_t(s.audioBitrateKbps) * 1000;
    m_actx->time_base = AVRational{1, rate};
    if (m_out->oformat->flags & AVFMT_GLOBALHEADER)
        m_actx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    int r = avcodec_open2(m_actx, codec, nullptr);
    if (r < 0) {
        if (error) *error = "Cannot open audio encoder: " + ff::errorString(r);
        return false;
    }
    m_ast = avformat_new_stream(m_out, nullptr);
    m_ast->time_base = m_actx->time_base;
    avcodec_parameters_from_context(m_ast->codecpar, m_actx);
    AVChannelLayout in;
    av_channel_layout_default(&in, m_channels);
    swr_alloc_set_opts2(&m_swr, &m_actx->ch_layout, m_actx->sample_fmt, rate, &in, AV_SAMPLE_FMT_FLT, m_rate, 0, nullptr);
    av_channel_layout_uninit(&in);
    if (!m_swr || swr_init(m_swr) < 0) {
        if (error) *error = "Cannot create audio converter";
        return false;
    }
    m_fifo = av_audio_fifo_alloc(m_actx->sample_fmt, m_channels, 4096);
    return true;
}

bool Encoder::encode(AVCodecContext *ctx, AVStream *st, AVFrame *f)
{
    int r = avcodec_send_frame(ctx, f);
    if (r < 0 && r != AVERROR_EOF)
        return false;
    ff::Packet pkt = ff::makePacket();
    for (;;) {
        r = avcodec_receive_packet(ctx, pkt.get());
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
            return true;
        if (r < 0)
            return false;
        av_packet_rescale_ts(pkt.get(), ctx->time_base, st->time_base);
        pkt->stream_index = st->index;
        if (av_interleaved_write_frame(m_out, pkt.get()) < 0)
            return false;
    }
}

bool Encoder::writeVideo(const uint16_t *rgba, int64_t frameIndex)
{
    if (!m_vctx)
        return true;
    if (av_frame_make_writable(m_vframe) < 0)
        return false;
    const uint8_t *src[1] = {reinterpret_cast<const uint8_t *>(rgba)};
    int srcStride[1] = {m_w * 8};
    sws_scale(m_sws, src, srcStride, 0, m_h, m_vframe->data, m_vframe->linesize);
    m_vframe->pts = frameIndex;
    return encode(m_vctx, m_vst, m_vframe);
}

bool Encoder::drainAudio(bool flush)
{
    const int fs = m_actx->frame_size > 0 && !(m_actx->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE) ? m_actx->frame_size : 1024;
    while (av_audio_fifo_size(m_fifo) >= fs || (flush && av_audio_fifo_size(m_fifo) > 0)) {
        int n = std::min(fs, av_audio_fifo_size(m_fifo));
        AVFrame *f = av_frame_alloc();
        f->nb_samples = n;
        f->format = m_actx->sample_fmt;
        av_channel_layout_copy(&f->ch_layout, &m_actx->ch_layout);
        f->sample_rate = m_actx->sample_rate;
        av_frame_get_buffer(f, 0);
        av_audio_fifo_read(m_fifo, (void **)f->data, n);
        f->pts = m_audioPts;
        m_audioPts += n;
        bool ok = encode(m_actx, m_ast, f);
        av_frame_free(&f);
        if (!ok)
            return false;
    }
    return true;
}

bool Encoder::writeAudio(const float *interleaved, int frames)
{
    if (!m_actx)
        return true;
    int outMax = swr_get_out_samples(m_swr, frames) + 64;
    AVFrame *tmp = av_frame_alloc();
    tmp->nb_samples = outMax;
    tmp->format = m_actx->sample_fmt;
    av_channel_layout_copy(&tmp->ch_layout, &m_actx->ch_layout);
    av_frame_get_buffer(tmp, 0);
    const uint8_t *in[1] = {reinterpret_cast<const uint8_t *>(interleaved)};
    int got = swr_convert(m_swr, tmp->data, outMax, in, frames);
    if (got > 0)
        av_audio_fifo_write(m_fifo, (void **)tmp->data, got);
    av_frame_free(&tmp);
    return drainAudio(false);
}

bool Encoder::finish(QString *error)
{
    bool ok = true;
    if (m_actx) {
        // Flush resampler.
        AVFrame *tmp = av_frame_alloc();
        tmp->nb_samples = 4096;
        tmp->format = m_actx->sample_fmt;
        av_channel_layout_copy(&tmp->ch_layout, &m_actx->ch_layout);
        av_frame_get_buffer(tmp, 0);
        int got = swr_convert(m_swr, tmp->data, 4096, nullptr, 0);
        if (got > 0)
            av_audio_fifo_write(m_fifo, (void **)tmp->data, got);
        av_frame_free(&tmp);
        ok = drainAudio(true) && encode(m_actx, m_ast, nullptr);
    }
    if (m_vctx)
        ok = encode(m_vctx, m_vst, nullptr) && ok;
    int r = av_write_trailer(m_out);
    if (r < 0) {
        ok = false;
        if (error) *error = ff::errorString(r);
    }
    return ok;
}

} // namespace rc
