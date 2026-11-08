#include "AudioPanel.h"
#include "Widgets.h"
#include "audio/AudioEffects.h"
#include "playback/Playback.h"
#include <QDial>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QScrollArea>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

namespace rc {

LevelMeter::LevelMeter(QWidget *parent) : QWidget(parent)
{
    setMinimumWidth(22);
    setMinimumHeight(80);
}

void LevelMeter::setLevels(float l, float r)
{
    // Fast attack, ~20 dB/s release.
    m_l = std::max(l, m_l * 0.85f);
    m_r = std::max(r, m_r * 0.85f);
    if (l >= m_holdL) { m_holdL = l; m_holdFramesL = 60; } else if (--m_holdFramesL <= 0) m_holdL *= 0.95f;
    if (r >= m_holdR) { m_holdR = r; m_holdFramesR = 60; } else if (--m_holdFramesR <= 0) m_holdR *= 0.95f;
    update();
}

void LevelMeter::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(18, 18, 20));
    const int H = height() - 4;
    auto yOf = [&](float lin) {
        const double db = lin > 0 ? 20 * std::log10(lin) : -100;
        const double f = std::clamp((db + 60.0) / 60.0, 0.0, 1.0);
        return 2 + int((1 - f) * H);
    };
    const int bw = (width() - 6) / 2;
    for (int c = 0; c < 2; ++c) {
        const float v = c == 0 ? m_l : m_r;
        const float hold = c == 0 ? m_holdL : m_holdR;
        const int x = 2 + c * (bw + 2);
        QLinearGradient g(0, 2 + H, 0, 2);
        g.setColorAt(0, QColor(40, 180, 90));
        g.setColorAt(0.75, QColor(220, 210, 60));
        g.setColorAt(0.9, QColor(240, 120, 40));
        g.setColorAt(1, QColor(240, 50, 50));
        const int y = yOf(v);
        p.fillRect(QRect(x, y, bw, 2 + H - y), g);
        p.fillRect(QRect(x, yOf(hold), bw, 2), hold >= 1.0f ? QColor(255, 60, 60) : QColor(230, 230, 230));
    }
    p.setPen(QColor(255, 255, 255, 40));
    for (int db = 0; db >= -54; db -= 6) {
        const int y = yOf(std::pow(10.0f, db / 20.0f));
        p.drawLine(0, y, 2, y);
    }
}

AudioMixerPanel::AudioMixerPanel(Playback *program, QWidget *parent) : QWidget(parent), m_playback(program)
{
    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    lay->addWidget(scroll, 1);
    m_strips = new QWidget;
    new QHBoxLayout(m_strips);
    scroll->setWidget(m_strips);
    m_master = new LevelMeter(this);
    m_master->setFixedWidth(30);
    lay->addWidget(m_master);
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] {
        if (m_playback) m_master->setLevels(m_playback->meterLeft(), m_playback->meterRight());
    });
    timer->start(33);
    Session *s = Session::instance();
    connect(&s->doc(), &Document::changed, this, &AudioMixerPanel::rebuild);
    connect(s, &Session::sequenceChanged, this, &AudioMixerPanel::rebuild);
    rebuild();
}

void AudioMixerPanel::rebuild()
{
    const Sequence *seq = Session::instance()->sequence();
    if (!seq) return;
    QString key = seq->id;
    for (const auto &t : seq->audioTracks)
        key += t.id + QString::number(t.volumeDb) + QString::number(t.pan) + (t.muted ? "m" : "") + (t.solo ? "s" : "") + t.name;
    key += QString::number(seq->masterVolumeDb);
    if (key == m_key) return;
    m_key = key;
    auto *lay = static_cast<QHBoxLayout *>(m_strips->layout());
    while (QLayoutItem *it = lay->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    for (int i = 0; i < (int)seq->audioTracks.size(); ++i)
        lay->addWidget(strip(i));
    lay->addWidget(strip(-1));
    lay->addStretch();
}

QWidget *AudioMixerPanel::strip(int ti)
{
    Session *s = Session::instance();
    const Sequence *seq = s->sequence();
    auto *w = new QWidget;
    w->setFixedWidth(78);
    w->setStyleSheet(ti < 0 ? "background: #2c2a33;" : "background: #27272c;");
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(3);
    const Track *t = ti >= 0 ? &seq->audioTracks[ti] : nullptr;
    auto setTrack = [s, ti](const QString &label, const QString &merge, const std::function<void(Sequence &)> &fn) {
        const QString seqId = s->project().activeSequenceId;
        s->doc().editMerged(label, merge, [&](Project &p) {
            Sequence *sq = p.sequence(seqId);
            if (!sq || (ti >= 0 && ti >= (int)sq->audioTracks.size())) return false;
            fn(*sq);
            return true;
        });
    };
    if (t) {
        auto *pan = new QDial(w);
        pan->setRange(-100, 100);
        pan->setValue(int(t->pan * 100));
        pan->setFixedSize(40, 40);
        pan->setNotchesVisible(true);
        pan->setToolTip(tr("Pan"));
        connect(pan, &QDial::valueChanged, w, [=](int val) { setTrack(tr("Track Pan"), "pan" + QString::number(ti), [=](Sequence &sq) { sq.audioTracks[ti].pan = val / 100.0; }); });
        connect(pan, &QDial::sliderReleased, w, [s] { s->doc().endMerge(); });
        v->addWidget(pan, 0, Qt::AlignHCenter);
        auto *row = new QHBoxLayout;
        auto *m = new QToolButton(w), *so = new QToolButton(w);
        m->setText("M");
        so->setText("S");
        m->setCheckable(true);
        so->setCheckable(true);
        m->setChecked(t->muted);
        so->setChecked(t->solo);
        connect(m, &QToolButton::clicked, w, [=](bool on) { setTrack(tr("Mute Track"), {}, [=](Sequence &sq) { sq.audioTracks[ti].muted = on; }); s->doc().endMerge(); });
        connect(so, &QToolButton::clicked, w, [=](bool on) { setTrack(tr("Solo Track"), {}, [=](Sequence &sq) { sq.audioTracks[ti].solo = on; }); s->doc().endMerge(); });
        row->addWidget(m);
        row->addWidget(so);
        v->addLayout(row);
    } else {
        v->addSpacing(44);
        v->addWidget(new QLabel(" "));
    }
    auto *db = new QLabel(w);
    db->setAlignment(Qt::AlignCenter);
    auto *fader = new QSlider(Qt::Vertical, w);
    fader->setRange(-600, 150);   // tenths of dB
    const double cur = t ? t->volumeDb : seq->masterVolumeDb;
    fader->setValue(int(std::lround(cur * 10)));
    db->setText(QString::number(cur, 'f', 1) + " dB");
    connect(fader, &QSlider::valueChanged, w, [=](int val) {
        const double d = val <= -600 ? -96.0 : val / 10.0;
        db->setText(QString::number(d, 'f', 1) + " dB");
        setTrack(tr("Track Volume"), "vol" + QString::number(ti), [=](Sequence &sq) {
            if (ti >= 0) sq.audioTracks[ti].volumeDb = d; else sq.masterVolumeDb = d;
        });
    });
    connect(fader, &QSlider::sliderReleased, w, [s] { s->doc().endMerge(); });
    v->addWidget(fader, 1, Qt::AlignHCenter);
    v->addWidget(db);
    auto *name = new QLabel(t ? t->name : tr("Master"), w);
    name->setAlignment(Qt::AlignCenter);
    name->setStyleSheet("font-weight: bold;");
    v->addWidget(name);
    return w;
}

} // namespace rc
