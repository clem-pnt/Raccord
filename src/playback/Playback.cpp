#include "Playback.h"
#include "audio/AudioMixer.h"
#include "media/MediaPool.h"
#include "ui/Session.h"
#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>
#include <cstring>

namespace rc {

// ---- RingDevice --------------------------------------------------------------

RingDevice::RingDevice(QObject *parent) : QIODevice(parent)
{
    m_buf.resize(48000 * 2 * 4 * 2);   // 2 s stereo float
    open(QIODevice::ReadOnly);
}

void RingDevice::clear()
{
    QMutexLocker l(&m_mutex);
    m_read = m_write = 0;
}

qint64 RingDevice::write(const float *data, qint64 frames, int channels)
{
    QMutexLocker l(&m_mutex);
    const qint64 bytes = frames * channels * 4;
    const qint64 cap = (qint64)m_buf.size();
    const qint64 free = cap - (m_write - m_read);
    const qint64 n = std::min(bytes, free);
    const char *src = reinterpret_cast<const char *>(data);
    for (qint64 i = 0; i < n;) {
        qint64 pos = m_write % cap;
        qint64 chunk = std::min(n - i, cap - pos);
        std::memcpy(m_buf.data() + pos, src + i, chunk);
        i += chunk;
        m_write += chunk;
    }
    return n / (channels * 4);
}

qint64 RingDevice::availableFrames(int channels) const
{
    QMutexLocker l(&m_mutex);
    return (m_write - m_read) / (channels * 4);
}

qint64 RingDevice::freeFrames(int channels) const
{
    QMutexLocker l(&m_mutex);
    return ((qint64)m_buf.size() - (m_write - m_read)) / (channels * 4);
}

qint64 RingDevice::readData(char *data, qint64 maxlen)
{
    QMutexLocker l(&m_mutex);
    const qint64 cap = (qint64)m_buf.size();
    qint64 avail = m_write - m_read;
    qint64 n = std::min(maxlen, avail);
    n -= n % 8;
    for (qint64 i = 0; i < n;) {
        qint64 pos = m_read % cap;
        qint64 chunk = std::min(n - i, cap - pos);
        std::memcpy(data + i, m_buf.data() + pos, chunk);
        i += chunk;
        m_read += chunk;
    }
    // Underrun: output silence instead of stalling the sink.
    if (n < maxlen) {
        qint64 pad = maxlen - n;
        pad -= pad % 8;
        std::memset(data + n, 0, pad);
        n += pad;
    }
    return n;
}

qint64 RingDevice::bytesAvailable() const
{
    return 1 << 16;
}

// ---- AudioProducer ----------------------------------------------------------------

AudioProducer::AudioProducer(RingDevice *ring, QObject *parent) : QThread(parent), m_ring(ring) {}

AudioProducer::~AudioProducer()
{
    stopProducing();
}

void AudioProducer::start(std::shared_ptr<const Project> project, const QString &sequenceId, const QString &mediaId, int64_t samplePos,
                          int rate, int channels)
{
    stopProducing();
    m_project = std::move(project);
    m_seq = sequenceId;
    m_media = mediaId;
    m_pos = samplePos;
    m_rate = rate;
    m_channels = channels;
    m_stop = false;
    QThread::start(QThread::TimeCriticalPriority);
}

void AudioProducer::stopProducing()
{
    m_stop = true;
    wait();
}

void AudioProducer::run()
{
    AudioMixer mixer;
    const int block = 1024;
    std::vector<float> buf(size_t(block) * m_channels);
    const Sequence *seq = m_project ? m_project->sequence(m_seq) : nullptr;
    const MediaItem *media = m_project ? m_project->mediaItem(m_media) : nullptr;
    Sequence copy;
    if (seq) {
        copy = *seq;
        copy.audioChannels = m_channels;
        copy.sampleRate = m_rate;
    }
    while (!m_stop) {
        if (m_ring->freeFrames(m_channels) < block) {
            msleep(5);
            continue;
        }
        if (seq)
            mixer.mix(*m_project, copy, m_pos, block, buf.data());
        else if (media)
            AudioMixer::mixMedia(*media, m_rate, m_channels, m_pos, block, buf.data());
        else
            std::fill(buf.begin(), buf.end(), 0.f);
        float l = 0, r = 0;
        for (int i = 0; i < block; ++i) {
            l = std::max(l, std::abs(buf[size_t(i) * m_channels]));
            r = std::max(r, std::abs(buf[size_t(i) * m_channels + (m_channels > 1 ? 1 : 0)]));
        }
        meterL = std::max(l, meterL.load() * 0.85f);
        meterR = std::max(r, meterR.load() * 0.85f);
        m_ring->write(buf.data(), block, m_channels);
        m_pos += block;
    }
}

// ---- Prefetcher ---------------------------------------------------------------------

Prefetcher::Prefetcher(QObject *parent) : QThread(parent) {}

Prefetcher::~Prefetcher()
{
    halt();
}

void Prefetcher::request(std::shared_ptr<const Project> project, const QString &sequenceId, Ticks from, double rate)
{
    {
        QMutexLocker l(&m_mutex);
        m_project = std::move(project);
        m_seq = sequenceId;
        m_from = from;
        m_rate = rate;
        ++m_generation;
    }
    if (!isRunning()) {
        m_stop = false;
        start(QThread::LowPriority);
    }
}

void Prefetcher::halt()
{
    m_stop = true;
    wait();
}

void Prefetcher::run()
{
    while (!m_stop) {
        std::shared_ptr<const Project> project;
        QString seqId;
        Ticks from;
        double rate;
        int gen;
        {
            QMutexLocker l(&m_mutex);
            project = m_project;
            seqId = m_seq;
            from = m_from;
            rate = m_rate;
            gen = m_generation;
        }
        const Sequence *seq = project ? project->sequence(seqId) : nullptr;
        if (!seq || rate == 0) {
            msleep(20);
            continue;
        }
        const Ticks ft = seq->fps.frameTicks();
        // Decode up to ~1.5 s ahead (in playback direction).
        const int ahead = std::max(8, int(seq->fps.fps() * 1.5));
        for (int k = 2; k < ahead && !m_stop && gen == m_generation; ++k) {
            const Ticks t = from + Ticks(double(k * ft) * rate);
            for (const auto &track : seq->videoTracks) {
                if (track.muted)
                    continue;
                int idx = track.clipIndexAt(t);
                if (idx < 0)
                    continue;
                const Clip &c = track.clips[idx];
                if (c.source != ClipSource::Media || !c.enabled)
                    continue;
                const MediaItem *m = project->mediaItem(c.mediaId);
                if (!m || m->info.isStill || !m->info.hasVideo)
                    continue;
                const QString path = (project->settings.useProxies && !m->proxyPath.isEmpty()) ? m->proxyPath : m->path;
                const Ticks st = c.sourceAt(t);
                const int64_t fi = ticksToFrame(st, m->fps());
                if (!MediaPool::instance().cache().contains(path, fi))
                    MediaPool::instance().videoFrame(path, st, m->fps());
            }
        }
        // Wait for a new request.
        while (!m_stop && gen == m_generation)
            msleep(10);
    }
}

// ---- Playback --------------------------------------------------------------------------

Playback::Playback(Mode mode, QObject *parent) : QObject(parent), m_mode(mode)
{
    m_timer.setInterval(8);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &Playback::tick);
    m_ring = new RingDevice(this);
    m_producer = new AudioProducer(m_ring, this);
    if (mode == Mode::Program)
        m_prefetch = new Prefetcher(this);
}

Playback::~Playback()
{
    stop();
    if (m_prefetch)
        m_prefetch->halt();
}

FrameRate Playback::fps() const
{
    Session *s = Session::instance();
    if (m_mode == Mode::Program)
        return s->sequence() ? s->sequence()->fps : FrameRate{25, 1};
    const MediaItem *m = s->project().mediaItem(s->sourceMedia());
    return m ? m->fps() : FrameRate{25, 1};
}

Ticks Playback::startPosition() const
{
    Session *s = Session::instance();
    return m_mode == Mode::Program ? s->playhead() : s->sourcePosition();
}

Ticks Playback::endPosition() const
{
    Session *s = Session::instance();
    if (m_mode == Mode::Program)
        return s->sequence() ? s->sequence()->duration() : 0;
    const MediaItem *m = s->project().mediaItem(s->sourceMedia());
    return m ? (m->info.isStill ? 5 * kTicksPerSecond : m->info.duration) : 0;
}

Ticks Playback::currentPosition() const
{
    return m_start + Ticks(double(m_clock.nsecsElapsed()) * 1e-9 * m_rate * double(kTicksPerSecond));
}

void Playback::setVolume(float v)
{
    m_volume = v;
    if (m_sink)
        m_sink->setVolume(v);
}

void Playback::play(double rate)
{
    Session *s = Session::instance();
    const bool wasPlaying = isPlaying();
    if (wasPlaying)
        m_start = currentPosition();
    else
        m_start = startPosition();
    // Restart from the beginning when at the end.
    if (!wasPlaying && rate > 0 && m_start >= endPosition() - fps().frameTicks())
        m_start = 0;
    m_rate = rate;
    m_clock.restart();
    // Audio only at normal forward speed.
    if (m_sink) {
        m_sink->stop();
        delete m_sink;
        m_sink = nullptr;
    }
    m_producer->stopProducing();
    m_ring->clear();
    if (rate == 1.0) {
        int sr = 48000, ch = 2;
        if (m_mode == Mode::Program && s->sequence())
            sr = s->sequence()->sampleRate;
        QAudioFormat fmt;
        fmt.setSampleRate(sr);
        fmt.setChannelCount(ch);
        fmt.setSampleFormat(QAudioFormat::Float);
        QAudioDevice dev = QMediaDevices::defaultAudioOutput();
        if (!dev.isNull() && dev.isFormatSupported(fmt)) {
            m_producer->start(s->snapshot(), m_mode == Mode::Program && s->sequence() ? s->sequence()->id : QString(),
                              m_mode == Mode::Source ? s->sourceMedia() : QString(), ticksToSamples(m_start, sr), sr, ch);
            // Give the producer a head start so playback starts in sync.
            for (int i = 0; i < 40 && m_ring->availableFrames(ch) < sr / 10; ++i)
                QThread::msleep(2);
            m_sink = new QAudioSink(dev, fmt, this);
            m_sink->setBufferSize(sr * ch * 4 / 12);   // ~80 ms
            m_sink->setVolume(m_volume);
            m_sink->start(m_ring);
            m_clock.restart();
        }
    }
    if (m_prefetch && s->sequence())
        m_prefetch->request(s->snapshot(), s->sequence()->id, m_start, rate);
    m_timer.start();
    if (!wasPlaying)
        emit stateChanged(true);
}

void Playback::stop()
{
    if (!isPlaying())
        return;
    Ticks pos = currentPosition();
    m_rate = 0;
    m_timer.stop();
    if (m_sink) {
        m_sink->stop();
        delete m_sink;
        m_sink = nullptr;
    }
    m_producer->stopProducing();
    if (m_prefetch)
        m_prefetch->request({}, {}, 0, 0);
    Session *s = Session::instance();
    pos = std::clamp<Ticks>(floorToFrame(pos, fps()), 0, std::max<Ticks>(0, endPosition()));
    if (m_mode == Mode::Program)
        s->setPlayhead(pos);
    else
        s->setSourcePosition(pos);
    emit positionChanged(pos);
    emit stateChanged(false);
}

void Playback::toggle()
{
    if (isPlaying())
        stop();
    else
        play(1.0);
}

void Playback::shuttleForward()
{
    static const double steps[] = {1, 2, 4, 8};
    if (m_rate <= 0) { play(1.0); return; }
    for (double s : steps)
        if (s > m_rate) { play(s); return; }
}

void Playback::shuttleBackward()
{
    static const double steps[] = {-1, -2, -4, -8};
    if (m_rate >= 0) { play(-1.0); return; }
    for (double s : steps)
        if (s < m_rate) { play(s); return; }
}

void Playback::tick()
{
    Session *s = Session::instance();
    Ticks pos = currentPosition();
    const Ticks end = endPosition();
    Ticks loopStart = 0, loopEnd = end;
    if (m_mode == Mode::Program && s->sequence()) {
        TimeRange wr = s->sequence()->workRange();
        if (m_loop) { loopStart = wr.start; loopEnd = wr.end; }
    }
    if (m_rate > 0 && pos >= loopEnd) {
        if (m_loop) {
            m_start = loopStart;
            m_clock.restart();
            pos = loopStart;
            if (m_rate == 1.0) play(1.0);
        } else {
            pos = end;
            m_start = pos;
            stop();
            return;
        }
    }
    if (m_rate < 0 && pos <= 0) {
        m_start = 0;
        stop();
        return;
    }
    const Ticks frame = floorToFrame(pos, fps());
    if (frame == m_lastEmitted)
        return;
    m_lastEmitted = frame;
    if (m_mode == Mode::Program) {
        s->setPlayhead(frame, true);
        if (m_prefetch && s->sequence())
            m_prefetch->request(s->snapshot(), s->sequence()->id, frame, m_rate);
    } else {
        s->setSourcePosition(frame);
    }
    emit positionChanged(frame);
}

} // namespace rc
