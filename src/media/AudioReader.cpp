#include "AudioReader.h"
#include <algorithm>
#include <cstring>
#include <functional>

namespace rc {

AudioReader::~AudioReader()
{
    if (m_codec)
        avcodec_free_context(&m_codec);
}

bool AudioReader::open(const QString &path, int streamOrdinal, int outRate, QString *error)
{
    m_path = path;
    m_outRate = outRate;
    m_blockSize = outRate;
    m_fmt = ff::openInput(path, error);
    if (!m_fmt)
        return false;
    int ordinal = 0;
    for (unsigned i = 0; i < m_fmt->nb_streams; ++i) {
        if (m_fmt->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;
        if (ordinal++ == streamOrdinal) {
            m_stream = (int)i;
            break;
        }
    }
    if (m_stream < 0)
        m_stream = av_find_best_stream(m_fmt.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (m_stream < 0) {
        if (error) *error = "No audio stream";
        return false;
    }
    for (unsigned i = 0; i < m_fmt->nb_streams; ++i)
        if ((int)i != m_stream)
            m_fmt->streams[i]->discard = AVDISCARD_ALL;
    AVStream *st = m_fmt->streams[m_stream];
    const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        if (error) *error = "No decoder";
        return false;
    }
    m_codec = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(m_codec, st->codecpar);
    m_codec->pkt_timebase = st->time_base;
    if (int r = avcodec_open2(m_codec, dec, nullptr); r < 0) {
        if (error) *error = ff::errorString(r);
        return false;
    }
    m_tb = st->time_base;
    m_startPts = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
    m_channels = std::clamp(m_codec->ch_layout.nb_channels, 1, 16);
    Ticks dur = 0;
    if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
        dur = rationalToTicks(st->duration, m_tb.num, m_tb.den);
    else if (m_fmt->duration != AV_NOPTS_VALUE)
        dur = rationalToTicks(m_fmt->duration, 1, AV_TIME_BASE);
    m_length = ticksToSamples(dur, m_outRate);

    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, m_channels);
    SwrContext *swr = nullptr;
    swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLT, m_outRate, &m_codec->ch_layout, m_codec->sample_fmt,
                        m_codec->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&outLayout);
    if (!swr || swr_init(swr) < 0) {
        if (error) *error = "Cannot initialise resampler";
        swr_free(&swr);
        return false;
    }
    m_swr.reset(swr);
    m_decPos = 0;
    return true;
}

void AudioReader::storeSamples(const float *data, int frames)
{
    // Distribute decoded samples into blocks.
    int offset = 0;
    while (offset < frames) {
        if (m_decPos < 0) {
            int skip = (int)std::min<int64_t>(-m_decPos, frames - offset);
            offset += skip;
            m_decPos += skip;
            continue;
        }
        const int64_t bi = m_decPos / m_blockSize;
        const int inBlock = int(m_decPos % m_blockSize);
        if (m_partialBlock != bi) {
            m_partialBlock = bi;
            m_partial.assign(size_t(m_blockSize) * m_channels, 0.f);
        }
        const int n = std::min(frames - offset, m_blockSize - inBlock);
        std::memcpy(m_partial.data() + size_t(inBlock) * m_channels, data + size_t(offset) * m_channels, sizeof(float) * n * m_channels);
        offset += n;
        m_decPos += n;
        if (m_decPos % m_blockSize == 0) {
            auto b = std::make_shared<std::vector<float>>(std::move(m_partial));
            m_blocks[bi] = b;
            m_lru.remove(bi);
            m_lru.push_front(bi);
            m_partial.clear();
            m_partialBlock = -1;
            while (m_lru.size() > m_maxBlocks) {
                m_blocks.erase(m_lru.back());
                m_lru.pop_back();
            }
        }
    }
}

void AudioReader::seekToBlock(int64_t blockIndex)
{
    // Pre-roll half a second so codecs with priming (AAC) are settled.
    Ticks t = samplesToTicks(blockIndex * m_blockSize, m_outRate) - kTicksPerSecond / 2;
    int64_t target = ticksToRational(std::max<Ticks>(0, t), m_tb.num, m_tb.den) + m_startPts;
    av_seek_frame(m_fmt.get(), m_stream, target, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(m_codec);
    swr_init(m_swr.get());
    m_eof = false;
    m_decPos = INT64_MIN;   // set by the first decoded frame
    m_partial.clear();
    m_partialBlock = -1;
}

bool AudioReader::decodeUntil(int64_t blockIndex)
{
    ff::Packet pkt = ff::makePacket();
    ff::Frame frame = ff::makeFrame();
    std::vector<float> buf;
    while (!m_blocks.count(blockIndex)) {
        int r = avcodec_receive_frame(m_codec, frame.get());
        if (r == 0) {
            if (m_decPos == INT64_MIN) {
                int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
                m_decPos = pts == AV_NOPTS_VALUE ? 0 : ticksToSamples(rationalToTicks(pts - m_startPts, m_tb.num, m_tb.den), m_outRate);
            }
            int outMax = swr_get_out_samples(m_swr.get(), frame->nb_samples) + 32;
            buf.resize(size_t(outMax) * m_channels);
            uint8_t *outPtr = reinterpret_cast<uint8_t *>(buf.data());
            int got = swr_convert(m_swr.get(), &outPtr, outMax, (const uint8_t **)frame->extended_data, frame->nb_samples);
            if (got > 0)
                storeSamples(buf.data(), got);
            av_frame_unref(frame.get());
            continue;
        }
        if (r == AVERROR_EOF) {
            // Flush the partial block so the tail is readable.
            if (m_partialBlock >= 0) {
                m_blocks[m_partialBlock] = std::make_shared<std::vector<float>>(m_partial);
                m_lru.push_front(m_partialBlock);
                if (m_decPos > m_length)
                    m_length = m_decPos;
                m_partialBlock = -1;
            }
            return m_blocks.count(blockIndex) > 0;
        }
        if (r != AVERROR(EAGAIN))
            return false;
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
        if (m_decPos != INT64_MIN && m_decPos > (blockIndex + 2) * int64_t(m_blockSize) && !m_blocks.count(blockIndex))
            return false;   // overshoot (bad seek): give up for this block
    }
    return true;
}

AudioReader::Block AudioReader::block(int64_t index)
{
    auto it = m_blocks.find(index);
    if (it != m_blocks.end()) {
        m_lru.remove(index);
        m_lru.push_front(index);
        return it->second;
    }
    if (!isOpen() || index < 0 || index * m_blockSize > m_length + m_blockSize)
        return {};
    // Sequential: decoder is at or just before this block.
    const bool sequential = m_decPos != INT64_MIN && m_decPos >= 0 && m_decPos <= index * int64_t(m_blockSize) &&
                            index * int64_t(m_blockSize) - m_decPos < 4 * int64_t(m_blockSize);
    if (!sequential || m_eof)
        seekToBlock(index);
    if (!decodeUntil(index)) {
        if (m_blocks.count(index))
            return m_blocks[index];
        // Second chance with a fresh seek.
        seekToBlock(index);
        decodeUntil(index);
    }
    it = m_blocks.find(index);
    return it == m_blocks.end() ? Block() : it->second;
}

void AudioReader::read(int64_t pos, int count, float *out)
{
    std::memset(out, 0, sizeof(float) * size_t(count) * m_channels);
    int done = 0;
    while (done < count) {
        int64_t p = pos + done;
        if (p < 0) {
            int skip = (int)std::min<int64_t>(-p, count - done);
            done += skip;
            continue;
        }
        if (p >= m_length + m_blockSize)
            break;
        int64_t bi = p / m_blockSize;
        int inBlock = int(p % m_blockSize);
        int n = std::min(count - done, m_blockSize - inBlock);
        Block b = block(bi);
        if (b) {
            const size_t avail = b->size() / m_channels;
            int copy = (int)std::min<int64_t>(n, int64_t(avail) - inBlock);
            if (copy > 0)
                std::memcpy(out + size_t(done) * m_channels, b->data() + size_t(inBlock) * m_channels, sizeof(float) * copy * m_channels);
        }
        done += n;
    }
}

bool AudioReader::scan(const std::function<bool(const float *, int, int64_t)> &cb)
{
    seekToBlock(0);
    m_decPos = 0;
    ff::Packet pkt = ff::makePacket();
    ff::Frame frame = ff::makeFrame();
    std::vector<float> buf;
    int64_t pos = 0;
    bool flushing = false;
    av_seek_frame(m_fmt.get(), m_stream, m_startPts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(m_codec);
    for (;;) {
        int r = avcodec_receive_frame(m_codec, frame.get());
        if (r == 0) {
            int outMax = swr_get_out_samples(m_swr.get(), frame->nb_samples) + 32;
            buf.resize(size_t(outMax) * m_channels);
            uint8_t *outPtr = reinterpret_cast<uint8_t *>(buf.data());
            int got = swr_convert(m_swr.get(), &outPtr, outMax, (const uint8_t **)frame->extended_data, frame->nb_samples);
            av_frame_unref(frame.get());
            if (got > 0) {
                if (!cb(buf.data(), got, pos))
                    return false;
                pos += got;
            }
            continue;
        }
        if (r == AVERROR_EOF)
            break;
        if (r != AVERROR(EAGAIN))
            break;
        if (flushing) {
            avcodec_send_packet(m_codec, nullptr);
            continue;
        }
        r = av_read_frame(m_fmt.get(), pkt.get());
        if (r < 0) {
            flushing = true;
            avcodec_send_packet(m_codec, nullptr);
            continue;
        }
        if (pkt->stream_index == m_stream)
            avcodec_send_packet(m_codec, pkt.get());
        av_packet_unref(pkt.get());
    }
    if (pos > 0)
        m_length = pos;
    // Reset decoding state for random access.
    m_decPos = INT64_MIN;
    m_eof = true;
    return true;
}

} // namespace rc
