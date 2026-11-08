#include "MediaCache.h"
#include "media/AudioReader.h"
#include "media/MediaPool.h"
#include "core/Time.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QMetaObject>
#include <QThreadPool>
#include <QtConcurrent>

namespace rc {

MediaCache *MediaCache::instance()
{
    static MediaCache *c = new MediaCache;
    return c;
}

MediaCache::MediaCache(QObject *parent) : QObject(parent)
{
    QThreadPool::globalInstance()->setMaxThreadCount(std::max(4, QThread::idealThreadCount()));
}

QImage MediaCache::thumbnail(const QString &path, int64_t ticks, int width)
{
    // Quantise to 1 s so scrolling reuses thumbnails.
    const int64_t q = ticks / kTicksPerSecond;
    const QString key = path + "|" + QString::number(q) + "|" + QString::number(width);
    {
        QMutexLocker l(&m_mutex);
        auto it = m_thumbs.find(key);
        if (it != m_thumbs.end())
            return *it;
        if (m_pendingThumbs.contains(key) || m_pendingThumbs.size() > 64)
            return {};
        m_pendingThumbs.insert(key);
    }
    (void)QtConcurrent::run([this, path, q, width, key] {
        QImage img = MediaPool::instance().thumbnail(path, q * kTicksPerSecond, width);
        {
            QMutexLocker l(&m_mutex);
            m_thumbs.insert(key, img.isNull() ? QImage(1, 1, QImage::Format_RGB32) : img);
            m_pendingThumbs.remove(key);
            if (m_thumbs.size() > 4000)
                m_thumbs.clear();
        }
        QMetaObject::invokeMethod(this, "updated", Qt::QueuedConnection);
    });
    return {};
}

std::shared_ptr<const Peaks> MediaCache::peaks(const QString &path, int stream)
{
    const QString key = path + "#" + QString::number(stream);
    {
        QMutexLocker l(&m_mutex);
        auto it = m_peaks.find(key);
        if (it != m_peaks.end())
            return *it;
        if (m_pendingPeaks.contains(key))
            return {};
        m_pendingPeaks.insert(key);
    }
    const QString dir = m_dir;
    (void)QtConcurrent::run([this, path, stream, key, dir] {
        auto pk = std::make_shared<Peaks>();
        QString file;
        if (!dir.isEmpty()) {
            QDir().mkpath(dir + "/peaks");
            file = dir + "/peaks/" + QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex().left(20) + ".pk";
            QFile f(file);
            if (f.open(QIODevice::ReadOnly)) {
                QByteArray b = f.readAll();
                pk->data.resize(b.size() / sizeof(float));
                std::memcpy(pk->data.data(), b.constData(), pk->data.size() * sizeof(float));
            }
        }
        if (pk->data.empty()) {
            AudioReader r;
            if (r.open(path, stream, 48000)) {
                const int win = 48000 / pk->perSecond;
                float cur = 0;
                int count = 0;
                const int ch = r.channels();
                r.scan([&](const float *d, int frames, int64_t) {
                    for (int i = 0; i < frames; ++i) {
                        float m = 0;
                        for (int c = 0; c < ch; ++c)
                            m = std::max(m, std::abs(d[size_t(i) * ch + c]));
                        cur = std::max(cur, m);
                        if (++count == win) {
                            pk->data.push_back(cur);
                            cur = 0;
                            count = 0;
                        }
                    }
                    return true;
                });
                if (!file.isEmpty()) {
                    QFile f(file);
                    if (f.open(QIODevice::WriteOnly))
                        f.write(reinterpret_cast<const char *>(pk->data.data()), qint64(pk->data.size() * sizeof(float)));
                }
            }
        }
        {
            QMutexLocker l(&m_mutex);
            m_peaks.insert(key, pk);
            m_pendingPeaks.remove(key);
        }
        QMetaObject::invokeMethod(this, "updated", Qt::QueuedConnection);
    });
    return {};
}

} // namespace rc
