#include "VideoDecoder.h"
#include <QFileInfo>
#include <QThread>
#include <functional>

namespace rc {

static enum AVPixelFormat s_hwFmtWanted = AV_PIX_FMT_NONE;

static enum AVPixelFormat getHwFormat(AVCodecContext *ctx, const enum AVPixelFormat *fmts)
{
    const int wanted = (int)(intptr_t)ctx->opaque;
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == wanted)
            return *p;
    // Fallback to the first software format.
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(*p);
        if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL))
            return *p;
    }
    return fmts[0];
}

VideoDecoder::~VideoDecoder() { close(); }

void VideoDecoder::close()
{
    if (m_codec)
        avcodec_free_context(&m_codec);
    m_fmt.reset();
    m_sws.reset();
    m_last.reset();
    m_pending.reset();
    m_stillFrame.reset();
    m_stream = -1;
    m_hwPixFmt = -1;
}

bool VideoDecoder::open(const QString &path, bool allowHardware, QString *error)
{
    close();
    m_path = path;
    m_fmt = ff::openInput(path, error);
    if (!m_fmt)
        return false;
    const AVCodec *dec = nullptr;
    m_stream = av_find_best_stream(m_fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (m_stream < 0 || !dec) {
        if (error) *error = "No video stream";
        return false;
    }
    AVStream *st = m_fmt->streams[m_stream];
    for (unsigned i = 0; i < m_fmt->nb_streams; ++i)
        if ((int)i != m_stream)
            m_fmt->streams[i]->discard = AVDISCARD_ALL;
    m_tb = st->time_base;
    m_startPts = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
    AVRational fr = st->avg_frame_rate.num > 0 ? st->avg_frame_rate : st->r_frame_rate;
    m_fps = fr.num > 0 && fr.den > 0 ? FrameRate::fromDouble(av_q2d(fr)) : FrameRate{25, 1};
    if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
        m_duration = ptsToTicks(st->duration + m_startPts);
    else if (m_fmt->duration != AV_NOPTS_VALUE)
        m_duration = rationalToTicks(m_fmt->duration, 1, AV_TIME_BASE);
    const QString container = QString::fromUtf8(m_fmt->iformat->name);
    m_still = (container.contains("_pipe") || container == "image2") && !path.contains('%') && st->nb_frames <= 1;

    m_codec = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(m_codec, st->codecpar);
    m_codec->pkt_timebase = st->time_base;
    m_codec->thread_count = 0;
    m_codec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    if (m_still)
        m_codec->thread_count = 1;

    const bool hwCodec = st->codecpar->codec_id == AV_CODEC_ID_H264 || st->codecpar->codec_id == AV_CODEC_ID_HEVC ||
                         st->codecpar->codec_id == AV_CODEC_ID_VP9 || st->codecpar->codec_id == AV_CODEC_ID_AV1;
    if (allowHardware && hwCodec && !m_still) {
        AVHWDeviceType type = ff::preferredHwType();
        if (AVBufferRef *dev = ff::hwDevice(type)) {
            for (int i = 0;; ++i) {
                const AVCodecHWConfig *cfg = avcodec_get_hw_config(dec, i);
                if (!cfg)
                    break;
                if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == type) {
                    m_hwPixFmt = cfg->pix_fmt;
                    m_codec->hw_device_ctx = av_buffer_ref(dev);
                    m_codec->opaque = (void *)(intptr_t)m_hwPixFmt;
                    m_codec->get_format = getHwFormat;
                    break;
                }
            }
        }
    }
    int r = avcodec_open2(m_codec, dec, nullptr);
    if (r < 0 && m_hwPixFmt != -1) {
        // Retry in software.
        avcodec_free_context(&m_codec);
        m_hwPixFmt = -1;
        return open(path, false, error);
    }
    if (r < 0) {
        if (error) *error = ff::errorString(r);
        close();
        return false;
    }
    if (st->sample_aspect_ratio.num > 0)
        m_par = av_q2d(st->sample_aspect_ratio);
    else if (st->codecpar->sample_aspect_ratio.num > 0)
        m_par = av_q2d(st->codecpar->sample_aspect_ratio);
    m_eof = false;
    m_lastRequest = kInvalidTicks;
    return true;
}

Ticks VideoDecoder::ptsToTicks(int64_t pts) const
{
    return rationalToTicks(pts - m_startPts, m_tb.num, m_tb.den);
}

bool VideoDecoder::seekTo(Ticks t)
{
    int64_t target = ticksToRational(std::max<Ticks>(0, t), m_tb.num, m_tb.den) + m_startPts;
    int r = av_seek_frame(m_fmt.get(), m_stream, target, AVSEEK_FLAG_BACKWARD);
    if (r < 0)
        r = av_seek_frame(m_fmt.get(), m_stream, target, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
    avcodec_flush_buffers(m_codec);
    m_last.reset();
    m_pending.reset();
    m_eof = false;
    return r >= 0;
}

VideoFramePtr VideoDecoder::wrap(AVFrame *src)
{
    std::shared_ptr<AVFrame> frame(av_frame_alloc(), [](AVFrame *f) { av_frame_free(&f); });
    if (src->format == m_hwPixFmt && m_hwPixFmt != -1) {
        if (av_hwframe_transfer_data(frame.get(), src, 0) < 0)
            return {};
        av_frame_copy_props(frame.get(), src);
    } else {
        av_frame_ref(frame.get(), src);
    }
    auto vf = std::make_shared<VideoFrame>();
    vf->width = frame->width;
    vf->height = frame->height;
    vf->pixelAspect = m_par;
    vf->rotation = m_rotation;
    int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
    vf->pts = pts == AV_NOPTS_VALUE ? 0 : ptsToTicks(pts);
    vf->duration = m_fps.frameTicks();
    vf->fullRange = frame->color_range == AVCOL_RANGE_JPEG;
    switch (frame->colorspace) {
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: vf->matrix = YuvMatrix::Bt601; break;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: vf->matrix = YuvMatrix::Bt2020; break;
    case AVCOL_SPC_RGB: vf->matrix = YuvMatrix::Identity; break;
    case AVCOL_SPC_BT709: vf->matrix = YuvMatrix::Bt709; break;
    default: vf->matrix = frame->height < 720 ? YuvMatrix::Bt601 : YuvMatrix::Bt709; break;
    }
    switch (frame->color_trc) {
    case AVCOL_TRC_SMPTE2084: vf->transfer = TransferFn::Pq; break;
    case AVCOL_TRC_ARIB_STD_B67: vf->transfer = TransferFn::Hlg; break;
    case AVCOL_TRC_LINEAR: vf->transfer = TransferFn::Linear; break;
    case AVCOL_TRC_IEC61966_2_1: vf->transfer = TransferFn::Srgb; break;
    default: vf->transfer = TransferFn::Bt709; break;
    }

    AVPixelFormat fmt = AVPixelFormat(frame->format);
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(fmt);
    bool native = false;
    if (d && !(d->flags & (AV_PIX_FMT_FLAG_BE | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_FLOAT))) {
        const int depth = d->comp[0].depth;
        const bool rgb = d->flags & AV_PIX_FMT_FLAG_RGB;
        const bool alpha = d->flags & AV_PIX_FMT_FLAG_ALPHA;
        if (!rgb && d->nb_components >= 3 && (d->flags & AV_PIX_FMT_FLAG_PLANAR) && depth <= 16) {
            if (d->comp[1].plane == 1 && d->comp[2].plane == 2 && d->comp[0].shift == 0) {
                vf->layout = PixelLayout::Planar;
                native = true;
            } else if (d->comp[1].plane == 1 && d->comp[2].plane == 1 && d->comp[1].offset < d->comp[2].offset) {
                vf->layout = PixelLayout::SemiPlanar;
                vf->msbAligned = d->comp[0].shift > 0;
                native = !alpha;
            }
            if (native) {
                vf->bitDepth = depth;
                vf->chromaShiftX = d->log2_chroma_w;
                vf->chromaShiftY = d->log2_chroma_h;
                vf->hasAlpha = alpha && vf->layout == PixelLayout::Planar;
                if (fmt == AV_PIX_FMT_YUVJ420P || fmt == AV_PIX_FMT_YUVJ422P || fmt == AV_PIX_FMT_YUVJ444P)
                    vf->fullRange = true;
            }
        } else if (!rgb && d->nb_components == 1 && (depth == 8 || depth == 16) && d->comp[0].shift == 0) {
            vf->layout = PixelLayout::Gray;
            vf->bitDepth = depth;
            vf->fullRange = true;
            native = true;
        } else if (fmt == AV_PIX_FMT_RGBA || fmt == AV_PIX_FMT_BGRA || fmt == AV_PIX_FMT_RGBA64LE) {
            vf->layout = PixelLayout::Rgba;
            vf->bitDepth = fmt == AV_PIX_FMT_RGBA64LE ? 16 : 8;
            vf->bgr = fmt == AV_PIX_FMT_BGRA;
            vf->hasAlpha = true;
            vf->fullRange = true;
            vf->matrix = YuvMatrix::Identity;
            if (vf->transfer == TransferFn::Bt709) vf->transfer = TransferFn::Srgb;
            native = true;
        }
    }
    if (!native) {
        // Convert to a layout the GPU path handles.
        const bool rgb = d && (d->flags & AV_PIX_FMT_FLAG_RGB);
        const bool alpha = d && (d->flags & AV_PIX_FMT_FLAG_ALPHA);
        const int depth = d ? d->comp[0].depth : 8;
        AVPixelFormat dst;
        if (rgb || (d && (d->flags & AV_PIX_FMT_FLAG_PAL)) || (d && (d->flags & AV_PIX_FMT_FLAG_FLOAT)))
            dst = depth > 8 ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGBA;
        else if (alpha)
            dst = AV_PIX_FMT_YUVA444P16LE;
        else if (depth > 8)
            dst = AV_PIX_FMT_YUV444P16LE;
        else
            dst = (d && d->log2_chroma_w && d->log2_chroma_h) ? AV_PIX_FMT_YUV420P : (d && d->log2_chroma_w ? AV_PIX_FMT_YUV422P : AV_PIX_FMT_YUV444P);
        if (!m_sws || m_swsSrcFmt != fmt || m_swsW != frame->width || m_swsH != frame->height || m_swsDstFmt != dst) {
            m_sws.reset(sws_getContext(frame->width, frame->height, fmt, frame->width, frame->height, dst, SWS_BICUBIC, nullptr, nullptr, nullptr));
            m_swsSrcFmt = fmt;
            m_swsW = frame->width;
            m_swsH = frame->height;
            m_swsDstFmt = dst;
        }
        if (!m_sws)
            return {};
        std::shared_ptr<AVFrame> conv(av_frame_alloc(), [](AVFrame *f) { av_frame_free(&f); });
        conv->format = dst;
        conv->width = frame->width;
        conv->height = frame->height;
        av_frame_get_buffer(conv.get(), 32);
        sws_scale(m_sws.get(), frame->data, frame->linesize, 0, frame->height, conv->data, conv->linesize);
        av_frame_copy_props(conv.get(), frame.get());
        frame = conv;
        const AVPixFmtDescriptor *dd = av_pix_fmt_desc_get(dst);
        if (dst == AV_PIX_FMT_RGBA || dst == AV_PIX_FMT_RGBA64LE) {
            vf->layout = PixelLayout::Rgba;
            vf->bitDepth = dst == AV_PIX_FMT_RGBA ? 8 : 16;
            vf->hasAlpha = alpha;
            vf->fullRange = true;
            vf->matrix = YuvMatrix::Identity;
            if (vf->transfer == TransferFn::Bt709) vf->transfer = TransferFn::Srgb;
        } else {
            vf->layout = PixelLayout::Planar;
            vf->bitDepth = dd->comp[0].depth;
            vf->chromaShiftX = dd->log2_chroma_w;
            vf->chromaShiftY = dd->log2_chroma_h;
            vf->hasAlpha = alpha;
        }
    }
    for (int i = 0; i < 4; ++i) {
        vf->data[i] = frame->data[i];
        vf->linesize[i] = frame->linesize[i];
    }
    vf->av = frame;
    return vf;
}

VideoFramePtr VideoDecoder::decodeNext(bool *eof)
{
    *eof = false;
    ff::Frame f = ff::makeFrame();
    ff::Packet pkt = ff::makePacket();
    for (;;) {
        int r = avcodec_receive_frame(m_codec, f.get());
        if (r == 0) {
            VideoFramePtr vf = wrap(f.get());
            if (vf && onFrameDecoded)
                onFrameDecoded(vf);
            return vf;
        }
        if (r == AVERROR_EOF) {
            *eof = true;
            return {};
        }
        if (r != AVERROR(EAGAIN))
            return {};
        if (m_eof) {
            avcodec_send_packet(m_codec, nullptr);
            continue;
        }
        r = av_read_frame(m_fmt.get(), pkt.get());
        if (r < 0) {
            m_eof = true;
            avcodec_send_packet(m_codec, nullptr);
            continue;
        }
        if (pkt->stream_index == m_stream)
            avcodec_send_packet(m_codec, pkt.get());
        av_packet_unref(pkt.get());
    }
}

VideoFramePtr VideoDecoder::frameAt(Ticks t)
{
    if (!isOpen())
        return {};
    if (m_still) {
        if (!m_stillFrame) {
            seekTo(0);
            bool eof = false;
            m_stillFrame = decodeNext(&eof);
        }
        return m_stillFrame;
    }
    const Ticks ft = m_fps.frameTicks();
    const Ticks half = ft / 2;
    t = std::max<Ticks>(0, t);
    if (m_duration > 0 && t >= m_duration)
        t = std::max<Ticks>(0, m_duration - ft);

    // Can we decode forward from the current position?
    bool forward = m_last && t >= m_last->pts - half && t < m_last->pts + 3 * kTicksPerSecond;
    if (m_last && t >= m_last->pts - half && t < m_last->pts + half)
        return m_last;
    if (m_pending && t >= m_pending->pts - half && t < m_pending->pts + half) {
        m_last = m_pending;
        m_pending.reset();
        return m_last;
    }
    if (!forward || (m_pending && t < m_pending->pts - half)) {
        // Need to seek. Some formats land after the target: back off if needed.
        Ticks back = 0;
        for (int attempt = 0; attempt < 4; ++attempt) {
            seekTo(t - back);
            bool eof = false;
            VideoFramePtr first = decodeNext(&eof);
            if (!first)
                break;
            if (first->pts <= t + half || t - back <= 0) {
                m_pending = first;
                break;
            }
            back = back ? back * 2 : kTicksPerSecond;
        }
    }
    VideoFramePtr prev = m_last;
    for (int guard = 0; guard < 100000; ++guard) {
        VideoFramePtr cur;
        if (m_pending) {
            cur = m_pending;
            m_pending.reset();
        } else {
            bool eof = false;
            cur = decodeNext(&eof);
            if (!cur) {
                m_last = prev;
                return prev;   // end of stream: hold last frame
            }
        }
        if (cur->pts > t + half) {
            m_pending = cur;
            m_last = prev ? prev : cur;
            return m_last;
        }
        prev = cur;
        if (cur->pts >= t - half) {
            m_last = cur;
            return cur;
        }
    }
    return prev;
}

VideoFramePtr VideoDecoder::keyframeNear(Ticks t)
{
    if (!isOpen())
        return {};
    if (m_still)
        return frameAt(0);
    seekTo(t);
    bool eof = false;
    VideoFramePtr f = decodeNext(&eof);
    m_last = f;
    return f;
}

} // namespace rc
