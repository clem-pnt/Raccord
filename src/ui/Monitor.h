#pragma once
#include "core/Model.h"
#include "playback/Playback.h"
#include <QOpenGLWidget>
#include <QWidget>
#include <memory>

class QLabel;
class QComboBox;
class QToolButton;
class QSlider;

namespace rc {

class Renderer;

class MonitorView : public QOpenGLWidget {
    Q_OBJECT
public:
    MonitorView(Playback::Mode mode, QWidget *parent = nullptr);
    ~MonitorView() override;
    void setResolutionDivider(int d) { m_divider = std::max(1, d); update(); }
    int resolutionDivider() const { return m_divider; }
    void setZoom(float z) { m_zoom = z; update(); }
    void setSafeMargins(bool on) { m_safe = on; update(); }
    bool safeMargins() const { return m_safe; }
    void setTransparencyGrid(bool on) { m_checker = on; update(); }
    void setScopesWanted(bool on) { m_scopes = on; }
    void setCompareFrame(const QImage &img) { m_compare = img; update(); }
    QImage grabCurrent();
    QSize contentSize() const;

signals:
    void frameForScopes(const QImage &img);
    void droppedFiles(const QStringList &paths);

protected:
    void initializeGL() override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private:
    QRectF videoRect() const;
    QRectF clipBounds(const Clip &c) const;   // selected clip box in widget coords
    Playback::Mode m_mode;
    std::unique_ptr<Renderer> m_renderer;
    int m_divider = 1;
    float m_zoom = 1.0f;
    bool m_safe = false;
    bool m_checker = false;
    bool m_scopes = false;
    qint64 m_lastScope = 0;
    QImage m_compare;
    // Direct manipulation of Motion
    bool m_dragging = false;
    int m_dragHandle = -1;   // -1 move, 0..3 corners (scale)
    QPointF m_dragStart;
    Value m_startPos, m_startScale;
};

class MonitorPanel : public QWidget {
    Q_OBJECT
public:
    MonitorPanel(Playback::Mode mode, QWidget *parent = nullptr);
    Playback *playback() { return m_playback; }
    MonitorView *view() { return m_view; }
    void stepFrames(int n);
    void markIn();
    void markOut();
    void clearInOut();
    void goToIn();
    void goToOut();
    Playback::Mode mode() const { return m_mode; }

signals:
    void insertRequested();
    void overwriteRequested();
    void exportFrameRequested();

private:
    void refresh();
    Playback::Mode m_mode;
    MonitorView *m_view;
    Playback *m_playback;
    QLabel *m_tc;
    QLabel *m_dur;
    QLabel *m_title;
    QSlider *m_scrub;
    QToolButton *m_playBtn;
    QComboBox *m_zoom;
    QComboBox *m_res;
    bool m_updatingScrub = false;
};

} // namespace rc
