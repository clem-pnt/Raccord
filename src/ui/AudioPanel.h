#pragma once
// Audio Track Mixer (faders, pan, mute/solo per track, master) and the
// master VU meter.
#include "Session.h"
#include <QWidget>

namespace rc {

class Playback;

class LevelMeter : public QWidget {
    Q_OBJECT
public:
    explicit LevelMeter(QWidget *parent = nullptr);
    void setLevels(float l, float r);   // linear peak
    QSize sizeHint() const override { return {26, 200}; }
protected:
    void paintEvent(QPaintEvent *) override;
private:
    float m_l = 0, m_r = 0, m_holdL = 0, m_holdR = 0;
    int m_holdFramesL = 0, m_holdFramesR = 0;
};

class AudioMixerPanel : public QWidget {
    Q_OBJECT
public:
    explicit AudioMixerPanel(Playback *program, QWidget *parent = nullptr);
private:
    void rebuild();
    QWidget *strip(int trackIndex);   // -1 = master
    Playback *m_playback;
    QWidget *m_strips = nullptr;
    QString m_key;
    LevelMeter *m_master = nullptr;
};

} // namespace rc
