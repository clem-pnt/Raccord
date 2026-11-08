#include "MediaPool.h"
#include "MediaProbe.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QThread>
#include <atomic>

namespace rc {

// ---- FrameCache ----------------------------------------------------------------

VideoFramePtr FrameCache::get(const QString &path, int64_t frame)
{
    QMutexLocker l(&m_mutex);
    auto it = m_map.find({path, frame});
    if (it == m_map.end())
        return {};
    m_lru.splice(m_lru.begin(), m_lru, it->second.lru);
    return it->second.frame;
}

bool FrameCache::contains(const QString &path, int64_t frame)
{
    QMutexLocker l(&m_mutex);
    return m_map.count({path, frame}) > 0;
}

void FrameCache::put(const QString &path, int64_t frame, const VideoFramePtr &f)
{
    if (!f)
        return;
    QMutexLocker l(&m_mutex);
    Key k{path, frame};
    auto it = m_map.find(k);
    if (it != m_map.end()) {
        m_lru.splice(m_lru.begin(), m_lru, it->second.lru);
        return;
    }
    m_lru.push_front(k);
    m_map[k] = {f, m_lru.begin()};
    m_used += f->byteSize();
    while (m_used > m_budget && !m_lru.empty()) {
        auto victim = m_map.find(m_lru.back());
        if (victim != m_map.end()) {
            m_used -= std::min(m_used, victim->second.frame->byteSize());
            m_map.erase(victim);
        }
        m_lru.pop_back();
    }
}

void FrameCache::clear()
{
    QMutexLocker l(&m_mutex);
    m_map.clear();
    m_lru.clear();
    m_used = 0;
}

void FrameCache::setBudget(size_t bytes)
{
    QMutexLocker l(&m_mutex);
    m_budget = bytes;
}

// ---- MediaPool -------------------------------------------------------------------

MediaPool &MediaPool::instance()
{
    static MediaPool p;
    return p;
}

MediaPool::MediaPool()
{
    if (const char *b = std::getenv("RACCORD_FRAME_CACHE_MB"))
        m_cache.setBudget(size_t(std::atoll(b)) << 20);
}

VideoDecoder *MediaPool::acquireDecoder(const QString &path, Ticks t)
{
    const std::string key = path.toStdString();
    std::shared_ptr<DecoderSlot> chosen;
    for (int wait = 0;; ++wait) {
        {
            QMutexLocker l(&m_mutex);
            auto &slotList = m_decoders[key];
            // Prefer a free decoder that can decode forward to t.
            for (auto &s : slotList)
                if (!s->busy && (!chosen || std::abs(t - s->lastUse) < std::abs(t - chosen->lastUse)))
                    chosen = s;
            if (!chosen && slotList.size() < 3) {
                auto s = std::make_shared<DecoderSlot>();
                s->dec = std::make_unique<VideoDecoder>();
                slotList.push_back(s);
                chosen = s;
                chosen->busy = true;
                l.unlock();
                QString err;
                if (!chosen->dec->open(path, m_hw, &err)) {
                    QMutexLocker l2(&m_mutex);
                    auto &slotList2 = m_decoders[key];
                    slotList2.erase(std::remove(slotList2.begin(), slotList2.end(), chosen), slotList2.end());
                    return nullptr;
                }
                chosen->dec->onFrameDecoded = [this, path, fps = chosen->dec->fps()](const VideoFramePtr &f) {
                    m_cache.put(path, ticksToFrame(f->pts + fps.frameTicks() / 2, fps), f);
                };
                return chosen->dec.get();
            }
            if (chosen) {
                chosen->busy = true;
                return chosen->dec.get();
            }
        }
        QThread::usleep(500);
        if (wait > 20000)
            return nullptr;
    }
}

void MediaPool::releaseDecoder(VideoDecoder *d)
{
    QMutexLocker l(&m_mutex);
    for (auto &[k, slotList] : m_decoders)
        for (auto &s : slotList)
            if (s->dec.get() == d) {
                s->busy = false;
                s->lastUse = QDateTime::currentMSecsSinceEpoch();
                return;
            }
}

VideoFramePtr MediaPool::image(const QString &path)
{
    const std::string key = path.toStdString();
    {
        QMutexLocker l(&m_mutex);
        auto it = m_images.find(key);
        if (it != m_images.end())
            return it->second;
    }
    VideoFramePtr f;
    QImage img;
    if (img.load(path) && !img.isNull()) {
        f = VideoFrame::fromImage(img);
    } else {
        VideoDecoder dec;
        if (dec.open(path, false))
            f = dec.frameAt(0);
    }
    QMutexLocker l(&m_mutex);
    m_images[key] = f;
    return f;
}

VideoFramePtr MediaPool::videoFrame(const QString &path, Ticks t, FrameRate fps, bool exact)
{
    if (path.isEmpty())
        return {};
    const int64_t idx = ticksToFrame(t, fps);
    if (VideoFramePtr f = m_cache.get(path, idx))
        return f;
    VideoDecoder *dec = acquireDecoder(path, t);
    if (!dec)
        return {};
    VideoFramePtr f;
    if (dec->isStill()) {
        f = dec->frameAt(0);
    } else {
        // Request the middle of the frame interval to be robust against pts jitter.
        f = exact ? dec->frameAt(frameToTicks(idx, fps) + fps.frameTicks() / 4) : dec->keyframeNear(t);
    }
    releaseDecoder(dec);
    if (f && exact)
        m_cache.put(path, idx, f);
    return f;
}

int MediaPool::audioChannels(const QString &path, int stream, int rate)
{
    std::vector<float> dummy;
    return readAudio(path, stream, rate, 0, 0, dummy);
}

int MediaPool::readAudio(const QString &path, int stream, int rate, int64_t pos, int count, std::vector<float> &out)
{
    const std::string key = path.toStdString() + "#" + std::to_string(stream) + "@" + std::to_string(rate);
    std::shared_ptr<AudioReader> r;
    {
        QMutexLocker l(&m_mutex);
        auto it = m_audio.find(key);
        if (it != m_audio.end()) {
            r = it->second;
        } else {
            r = std::make_shared<AudioReader>();
            m_audio[key] = r;
            QMutexLocker rl(&r->mutex);
            l.unlock();
            if (!r->open(path, stream, rate)) {
                QMutexLocker l2(&m_mutex);
                m_audio[key] = std::make_shared<AudioReader>();   // remember failure (not open)
                return 0;
            }
        }
    }
    QMutexLocker rl(&r->mutex);
    if (!r->isOpen())
        return 0;
    const int ch = r->channels();
    out.resize(size_t(count) * ch);
    if (count > 0)
        r->read(pos, count, out.data());
    return ch;
}

QImage MediaPool::thumbnail(const QString &path, Ticks t, int width)
{
    QString cacheFile;
    if (!m_cacheDir.isEmpty()) {
        QByteArray h = QCryptographicHash::hash((path + "|" + QString::number(t / (kTicksPerSecond / 4)) + "|" + QString::number(width)).toUtf8(),
                                                QCryptographicHash::Sha1).toHex().left(20);
        QDir().mkpath(m_cacheDir + "/thumbs");
        cacheFile = m_cacheDir + "/thumbs/" + h + ".jpg";
        QImage cached;
        if (QFileInfo::exists(cacheFile) && cached.load(cacheFile))
            return cached;
    }
    VideoFramePtr f;
    MediaInfo info;
    QImage img;
    if (img.load(path)) {
        img = img.scaledToWidth(width, Qt::SmoothTransformation);
    } else {
        VideoDecoder dec;
        if (!dec.open(path, false))
            return {};
        f = dec.keyframeNear(t);
        if (!f)
            return {};
        img = f->toImage(width * 2).scaledToWidth(width, Qt::SmoothTransformation);
    }
    if (!cacheFile.isEmpty() && !img.isNull())
        img.save(cacheFile, "JPG", 80);
    return img;
}

void MediaPool::trim(int keepPerFile)
{
    QMutexLocker l(&m_mutex);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto &[k, slotList] : m_decoders) {
        int kept = 0;
        slotList.erase(std::remove_if(slotList.begin(), slotList.end(), [&](const std::shared_ptr<DecoderSlot> &s) {
            if (s->busy) return false;
            if (++kept <= keepPerFile && now - s->lastUse < 120000) return false;
            return true;
        }), slotList.end());
    }
}

void MediaPool::forget(const QString &path)
{
    QMutexLocker l(&m_mutex);
    m_decoders.erase(path.toStdString());
    m_images.erase(path.toStdString());
}

} // namespace rc
