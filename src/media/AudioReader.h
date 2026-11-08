#pragma once
#include "FFmpeg.h"
#include "core/Time.h"
#include <QMutex>
#include <QString>
#include <list>
#include <map>
#include <memory>
#include <vector>
#include <functional>

namespace rc {

// Random access, sample accurate audio reader. Decodes in 1 second blocks
// resampled to a fixed output rate (interleaved float, native channel count),
// with an LRU block cache. Sequential reads never seek.
class AudioReader {
public:
    AudioReader() = default;
    ~AudioReader();
    bool open(const QString &path, int streamOrdinal, int outRate, QString *error = nullptr);
    bool isOpen() const { return m_codec != nullptr; }
    int channels() const { return m_channels; }
    int sampleRate() const { return m_outRate; }
    int64_t lengthSamples() const { return m_length; }
    // Reads `count` frames starting at `pos` (output-rate samples). Fills zeros outside the media.
    void read(int64_t pos, int count, float *out);
    // Decodes the whole stream sequentially (used for waveform / loudness analysis).
    // The callback receives interleaved blocks; return false to abort.
    bool scan(const std::function<bool(const float *, int frames, int64_t pos)> &cb);

    QMutex mutex;

private:
    using Block = std::shared_ptr<std::vector<float>>;
    Block block(int64_t index);
    bool decodeUntil(int64_t blockIndex);
    void seekToBlock(int64_t blockIndex);
    void storeSamples(const float *data, int frames);

    QString m_path;
    ff::FormatCtx m_fmt;
    AVCodecContext *m_codec = nullptr;
    ff::Swr m_swr;
    int m_stream = -1;
    AVRational m_tb{1, 1};
    int64_t m_startPts = 0;
    int m_outRate = 48000;
    int m_channels = 2;
    int64_t m_length = 0;
    int m_blockSize = 48000;
    // decoding state
    int64_t m_decPos = -1;           // output sample position of the next decoded sample
    bool m_eof = false;
    std::vector<float> m_partial;    // samples of the block being filled
    int64_t m_partialBlock = -1;
    // cache
    std::map<int64_t, Block> m_blocks;
    std::list<int64_t> m_lru;
    size_t m_maxBlocks = 180;
};

} // namespace rc
