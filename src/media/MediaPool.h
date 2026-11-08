#pragma once
// Shared access to decoders and decoded frames.
#include "VideoDecoder.h"
#include "AudioReader.h"
#include <QHash>
#include <QMutex>
#include <QImage>
#include <list>
#include <memory>
#include <unordered_map>

namespace rc {

// LRU cache of decoded frames keyed by (file, frame index).
class FrameCache {
public:
    explicit FrameCache(size_t budgetBytes = size_t(1536) << 20) : m_budget(budgetBytes) {}
    VideoFramePtr get(const QString &path, int64_t frame);
    void put(const QString &path, int64_t frame, const VideoFramePtr &f);
    bool contains(const QString &path, int64_t frame);
    void clear();
    void setBudget(size_t bytes);
    size_t usedBytes() const { return m_used; }

private:
    struct Key {
        QString path;
        int64_t frame;
        bool operator==(const Key &o) const { return frame == o.frame && path == o.path; }
    };
    struct KeyHash {
        size_t operator()(const Key &k) const { return qHash(k.path) ^ std::hash<int64_t>()(k.frame * 2654435761u); }
    };
    struct Entry { VideoFramePtr frame; std::list<Key>::iterator lru; };
    QMutex m_mutex;
    std::unordered_map<Key, Entry, KeyHash> m_map;
    std::list<Key> m_lru;
    size_t m_used = 0;
    size_t m_budget;
};

class MediaPool {
public:
    static MediaPool &instance();
    MediaPool();

    // Frame of `path` at media time t. Thread safe. `preferProxy` selects the proxy file when given.
    VideoFramePtr videoFrame(const QString &path, Ticks t, FrameRate fps, bool exact = true);
    // Interleaved float audio at `rate` for an audio stream. Returns channel count.
    int readAudio(const QString &path, int stream, int rate, int64_t pos, int count, std::vector<float> &out);
    int audioChannels(const QString &path, int stream, int rate);
    // Still images are decoded once and kept.
    VideoFramePtr image(const QString &path);
    // Small RGB thumbnail (cached on disk under cacheDir if set).
    QImage thumbnail(const QString &path, Ticks t, int width = 160);

    FrameCache &cache() { return m_cache; }
    void setCacheDir(const QString &dir) { m_cacheDir = dir; }
    QString cacheDir() const { return m_cacheDir; }
    void setHardwareDecoding(bool on) { m_hw = on; }
    // Releases decoders not used for a while (called periodically).
    void trim(int keepPerFile = 2);
    void forget(const QString &path);

private:
    struct DecoderSlot {
        std::unique_ptr<VideoDecoder> dec;
        qint64 lastUse = 0;
        bool busy = false;
    };
    VideoDecoder *acquireDecoder(const QString &path, Ticks t);
    void releaseDecoder(VideoDecoder *d);

    QMutex m_mutex;
    std::unordered_map<std::string, std::vector<std::shared_ptr<DecoderSlot>>> m_decoders;
    std::unordered_map<std::string, std::shared_ptr<AudioReader>> m_audio;
    std::unordered_map<std::string, VideoFramePtr> m_images;
    FrameCache m_cache;
    QString m_cacheDir;
    bool m_hw = true;
    std::atomic<qint64> m_clock{0};
};

} // namespace rc
