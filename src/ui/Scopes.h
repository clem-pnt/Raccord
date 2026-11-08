#pragma once
// Lumetri style scopes: waveform, RGB parade, vectorscope, histogram.
#include <QImage>
#include <QWidget>

class QComboBox;

namespace rc {

class ScopeView : public QWidget {
    Q_OBJECT
public:
    enum Mode { Waveform, Parade, Vectorscope, Histogram };
    explicit ScopeView(QWidget *parent = nullptr);
    void setMode(Mode m) { m_mode = m; recompute(); }
    void setFrame(const QImage &img);
protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override { recompute(); }
private:
    void recompute();
    Mode m_mode = Waveform;
    QImage m_frame;
    QImage m_plot;
};

class ScopesPanel : public QWidget {
    Q_OBJECT
public:
    explicit ScopesPanel(QWidget *parent = nullptr);
    ScopeView *view() { return m_view; }
public slots:
    void setFrame(const QImage &img) { m_view->setFrame(img); }
private:
    ScopeView *m_view;
    QComboBox *m_mode;
};

} // namespace rc
