#pragma once
// Asynchronous thumbnails and audio waveform peaks for the UI.
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <memory>
#include <vector>

namespace rc {

struct Peaks {
    int perSecond = 200;
    std::vector<float> data;   // max abs per window (mono)
};

class MediaCache : public QObject {
    Q_OBJECT
public:
    static MediaCache *instance();
    // Returns a null image while the thumbnail is being generated.
    QImage thumbnail(const QString &path, int64_t ticks, int width = 96);
    std::shared_ptr<const Peaks> peaks(const QString &path, int stream = 0);
    void setCacheDir(const QString &dir) { m_dir = dir; }

signals:
    void updated();

private:
    explicit MediaCache(QObject *parent = nullptr);
    QMutex m_mutex;
    QHash<QString, QImage> m_thumbs;
    QSet<QString> m_pendingThumbs;
    QHash<QString, std::shared_ptr<const Peaks>> m_peaks;
    QSet<QString> m_pendingPeaks;
    QString m_dir;
    int m_running = 0;
};

} // namespace rc
