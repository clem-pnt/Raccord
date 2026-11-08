#pragma once
// Real-time playback: system clock driven video, audio rendered ahead on a
// worker thread into a ring buffer consumed by QAudioSink, frame prefetching.
#include "core/Model.h"
#include <QElapsedTimer>
#include <QIODevice>
#include <QMutex>
#include <QObject>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <memory>
#include <vector>

class QAudioSink;

namespace rc {

class AudioMixer;

class RingDevice : public QIODevice {
    Q_OBJECT
public:
    explicit RingDevice(QObject *parent = nullptr);
    void clear();
    qint64 write(const float *data, qint64 frames, int channels);
    qint64 availableFrames(int channels) const;
    qint64 freeFrames(int channels) const;
protected:
    qint64 readData(char *data, qint64 maxlen) override;
    qint64 writeData(const char *, qint64) override { return -1; }
    qint64 bytesAvailable() const override;
    bool isSequential() const override { return true; }
private:
    mutable QMutex m_mutex;
    std::vector<char> m_buf;
    qint64 m_read = 0, m_write = 0;
};

class AudioProducer : public QThread {
    Q_OBJECT
public:
    AudioProducer(RingDevice *ring, QObject *parent = nullptr);
    ~AudioProducer() override;
    void start(std::shared_ptr<const Project> project, const QString &sequenceId, const QString &mediaId, int64_t samplePos, int rate, int channels);
    void stopProducing();
    std::atomic<float> meterL{0}, meterR{0};
protected:
    void run() override;
private:
    RingDevice *m_ring;
    std::shared_ptr<const Project> m_project;
    QString m_seq, m_media;
    int64_t m_pos = 0;
    int m_rate = 48000, m_channels = 2;
    std::atomic<bool> m_stop{false};
};

class Prefetcher : public QThread {
    Q_OBJECT
public:
    explicit Prefetcher(QObject *parent = nullptr);
    ~Prefetcher() override;
    void request(std::shared_ptr<const Project> project, const QString &sequenceId, Ticks from, double rate);
    void halt();
protected:
    void run() override;
private:
    QMutex m_mutex;
    std::shared_ptr<const Project> m_project;
    QString m_seq;
    Ticks m_from = 0;
    double m_rate = 1;
    std::atomic<bool> m_stop{false};
    std::atomic<int> m_generation{0};
};

class Playback : public QObject {
    Q_OBJECT
public:
    enum class Mode { Program, Source };
    explicit Playback(Mode mode, QObject *parent = nullptr);
    ~Playback() override;

    bool isPlaying() const { return m_rate != 0; }
    double rate() const { return m_rate; }
    void play(double rate = 1.0);
    void stop();
    void toggle();
    // Premiere style J/K/L shuttle.
    void shuttleForward();
    void shuttleBackward();
    void setLoop(bool on) { m_loop = on; }
    bool loop() const { return m_loop; }
    void setVolume(float v);
    float meterLeft() const { return m_producer ? m_producer->meterL.load() : 0.f; }
    float meterRight() const { return m_producer ? m_producer->meterR.load() : 0.f; }

signals:
    void positionChanged(Ticks t);   // ~60 Hz while playing
    void stateChanged(bool playing);

private:
    void tick();
    Ticks currentPosition() const;
    Ticks startPosition() const;
    Ticks endPosition() const;
    FrameRate fps() const;

    Mode m_mode;
    double m_rate = 0;
    Ticks m_start = 0;
    QElapsedTimer m_clock;
    QTimer m_timer;
    bool m_loop = false;
    RingDevice *m_ring = nullptr;
    AudioProducer *m_producer = nullptr;
    Prefetcher *m_prefetch = nullptr;
    QAudioSink *m_sink = nullptr;
    float m_volume = 1.0f;
    Ticks m_lastEmitted = kInvalidTicks;
};

} // namespace rc
