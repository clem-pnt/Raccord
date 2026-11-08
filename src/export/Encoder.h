#pragma once
#include "ExportSettings.h"
#include "media/FFmpeg.h"
#include <QString>
#include <vector>

struct AVAudioFifo;

namespace rc {

// Muxes encoded video (from RGBA16 frames) and audio (interleaved float) to a file.
class Encoder {
public:
    ~Encoder();
    bool open(const ExportSettings &s, int width, int height, FrameRate fps, int sampleRate, int channels, QString *error);
    // rgba: width*height*4 uint16, rows top first, straight alpha.
    bool writeVideo(const uint16_t *rgba, int64_t frameIndex);
    bool writeAudio(const float *interleaved, int frames);
    bool finish(QString *error = nullptr);
    bool hasVideo() const { return m_vctx != nullptr; }
    bool hasAudio() const { return m_actx != nullptr; }

private:
    bool openVideo(const ExportSettings &s, QString *error);
    bool openAudio(const ExportSettings &s, QString *error);
    bool encode(AVCodecContext *ctx, AVStream *st, AVFrame *f);
    bool drainAudio(bool flush);

    ff::FormatCtx m_fmt;
    AVFormatContext *m_out = nullptr;
    AVCodecContext *m_vctx = nullptr;
    AVCodecContext *m_actx = nullptr;
    AVStream *m_vst = nullptr;
    AVStream *m_ast = nullptr;
    SwsContext *m_sws = nullptr;
    SwrContext *m_swr = nullptr;
    AVAudioFifo *m_fifo = nullptr;
    AVFrame *m_vframe = nullptr;
    int m_w = 0, m_h = 0, m_rate = 48000, m_channels = 2;
    FrameRate m_fps;
    int64_t m_audioPts = 0;
    bool m_imageSeq = false;
    QString m_error;
};

} // namespace rc
