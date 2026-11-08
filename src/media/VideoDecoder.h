#pragma once
#include "VideoFrame.h"
#include "core/Model.h"
#include "FFmpeg.h"
#include <QMutex>
#include <QString>
#include <functional>

namespace rc {

// Frame accurate video decoder for one file. Not thread safe by itself: the
// DecoderPool serialises access with a per decoder mutex.
class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder &) = delete;
    VideoDecoder &operator=(const VideoDecoder &) = delete;

    bool open(const QString &path, bool allowHardware = true, QString *error = nullptr);
    void close();
    bool isOpen() const { return m_codec != nullptr; }
    const QString &path() const { return m_path; }
    FrameRate fps() const { return m_fps; }
    Ticks duration() const { return m_duration; }
    bool isStill() const { return m_still; }
    bool usingHardware() const { return m_hwPixFmt != -1; }

    // Frame displayed at media time t (0 = first frame). Decodes forward when
    // possible, seeks otherwise.
    VideoFramePtr frameAt(Ticks t);
    // Nearest keyframe (fast, for thumbnails while scrubbing).
    VideoFramePtr keyframeNear(Ticks t);
    // Callback for every decoded frame (used to fill the frame cache during playback).
    std::function<void(const VideoFramePtr &)> onFrameDecoded;

    QMutex mutex;   // held by users of the decoder

private:
    bool seekTo(Ticks t);
    VideoFramePtr decodeNext(bool *eof);
    VideoFramePtr wrap(AVFrame *f);
    Ticks ptsToTicks(int64_t pts) const;

    QString m_path;
    ff::FormatCtx m_fmt;
    AVCodecContext *m_codec = nullptr;
    int m_stream = -1;
    AVRational m_tb{1, 1};
    int64_t m_startPts = 0;
    FrameRate m_fps;
    Ticks m_duration = 0;
    bool m_still = false;
    int m_hwPixFmt = -1;
    ff::Sws m_sws;
    int m_swsSrcFmt = -1, m_swsW = 0, m_swsH = 0;
    int m_swsDstFmt = -1;
    VideoFramePtr m_last;     // last returned/decoded frame
    VideoFramePtr m_pending;  // decoded but beyond the requested time
    bool m_eof = false;
    Ticks m_lastRequest = kInvalidTicks;
    VideoFramePtr m_stillFrame;
    int m_rotation = 0;
    double m_par = 1.0;
};

} // namespace rc
