#pragma once
// Thin RAII helpers around FFmpeg.
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/display.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/timecode.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}
#include <QString>
#include <memory>

namespace rc::ff {

struct FormatCtxDeleter { void operator()(AVFormatContext *c) const { avformat_close_input(&c); } };
struct CodecCtxDeleter { void operator()(AVCodecContext *c) const { avcodec_free_context(&c); } };
struct FrameDeleter { void operator()(AVFrame *f) const { av_frame_free(&f); } };
struct PacketDeleter { void operator()(AVPacket *p) const { av_packet_free(&p); } };
struct SwsDeleter { void operator()(SwsContext *s) const { sws_freeContext(s); } };
struct SwrDeleter { void operator()(SwrContext *s) const { swr_free(&s); } };
struct BufferRefDeleter { void operator()(AVBufferRef *b) const { av_buffer_unref(&b); } };

using FormatCtx = std::unique_ptr<AVFormatContext, FormatCtxDeleter>;
using CodecCtx = std::unique_ptr<AVCodecContext, CodecCtxDeleter>;
using Frame = std::unique_ptr<AVFrame, FrameDeleter>;
using Packet = std::unique_ptr<AVPacket, PacketDeleter>;
using Sws = std::unique_ptr<SwsContext, SwsDeleter>;
using Swr = std::unique_ptr<SwrContext, SwrDeleter>;

inline Frame makeFrame() { return Frame(av_frame_alloc()); }
inline Packet makePacket() { return Packet(av_packet_alloc()); }

QString errorString(int err);
// Opens an input file (with image sequence support for patterns like name_%05d.png).
FormatCtx openInput(const QString &path, QString *error = nullptr);
// Silences FFmpeg logging below warnings (call once).
void initLogging();

// Hardware decoding: returns an AVBufferRef* device for the preferred API or nullptr.
AVBufferRef *hwDevice(AVHWDeviceType type);
AVHWDeviceType preferredHwType();   // from RACCORD_HWACCEL env or settings: vaapi/cuda/none

} // namespace rc::ff
