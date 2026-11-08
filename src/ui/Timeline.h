#pragma once
#include "Session.h"
#include <QWidget>
#include <optional>

class QScrollBar;

namespace rc {

class TimelineView : public QWidget {
    Q_OBJECT
public:
    explicit TimelineView(QWidget *parent = nullptr);
    void zoomIn();
    void zoomOut();
    void zoomToFit();
    void ensurePlayheadVisible();
    double pixelsPerSecond() const { return m_pps; }

    // Mime types used for drag and drop.
    static constexpr const char *kMediaMime = "application/x-raccord-media";
    static constexpr const char *kEffectMime = "application/x-raccord-effect";

signals:
    void openNested(const QString &sequenceId);
    void requestSpeedDialog(const QString &clipId);
    void requestImport(const QStringList &paths, Ticks at, int videoTrack, int audioTrack);
    void requestClipMenuAction(const QString &action);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dragMoveEvent(QDragMoveEvent *) override;
    void dragLeaveEvent(QDragLeaveEvent *) override;
    void dropEvent(QDropEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void keyPressEvent(QKeyEvent *) override;

private:
    struct Row {
        enum Kind { Caption, Video, Audio } kind;
        int index;
        int y, h;
    };
    struct Hit {
        enum Part { None, Ruler, Header, Empty, ClipBody, ClipIn, ClipOut, Transition, TransitionEdge, Caption, CaptionIn, CaptionOut, VolumeLine } part = None;
        int row = -1;
        QString clipId;
        QString transitionId;
        int captionIndex = -1;
        Ticks time = 0;
    };
    enum class Drag { None, Scrub, Move, TrimIn, TrimOut, Roll, Slip, Slide, Stretch, Marquee, Hand, Volume, TransitionDur, CaptionMove, CaptionTrim };

    std::vector<Row> rows() const;
    const Row *rowAt(int y) const;
    Hit hitTest(QPoint p) const;
    Ticks xToTicks(double x) const;
    double ticksToX(Ticks t) const;
    Ticks snap(Ticks t, const std::set<QString> &exclude, bool *snapped = nullptr) const;
    int headerWidth() const { return 190; }
    int rulerHeight() const { return 30; }
    int trackAreaTop() const { return rulerHeight(); }
    QRect clipRect(const Row &r, const Clip &c) const;
    void paintClip(QPainter &p, const Row &row, const Track &track, const Clip &c, bool selected);
    void paintHeaders(QPainter &p, const std::vector<Row> &rs);
    void paintRuler(QPainter &p);
    void headerClick(const Row &r, QPoint pos, Qt::MouseButton b);
    void updateScrollbars();
    void applyDrag(QPointF pos, bool commit);
    const Sequence *viewSequence() const;   // preview during drags, else active
    void clipMenu(const QString &clipId, QPoint global, Ticks at);

    double m_pps = 40;          // pixels per second
    Ticks m_scroll = 0;         // time at the left of the clip area
    int m_scrollY = 0;
    QScrollBar *m_hbar;
    QScrollBar *m_vbar;
    // Drag state
    Drag m_drag = Drag::None;
    QPoint m_pressPos;
    Hit m_pressHit;
    Ticks m_pressTime = 0;
    std::optional<Sequence> m_origin;    // sequence at drag start
    std::optional<Sequence> m_preview;   // result while dragging
    QRect m_marquee;
    Ticks m_snapLine = kInvalidTicks;
    int m_dropRow = -1;
    Ticks m_dropTime = kInvalidTicks;
    Ticks m_dropDuration = 0;
    double m_pressValue = 0;
    QString m_trimClip;
};

class TimelinePanel : public QWidget {
    Q_OBJECT
public:
    explicit TimelinePanel(QWidget *parent = nullptr);
    TimelineView *view() { return m_view; }
private:
    TimelineView *m_view;
};

} // namespace rc
