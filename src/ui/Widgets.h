#pragma once
// Reusable controls: scrubbable numbers, colour swatch, curves editor, colour wheel.
#include "core/Param.h"
#include <QWidget>
#include <vector>

namespace rc {

// Premiere style "hot text" number: drag horizontally to change, click to type.
class ValueSlider : public QWidget {
    Q_OBJECT
public:
    explicit ValueSlider(QWidget *parent = nullptr);
    void setRange(double min, double max, bool soft = true) { m_min = min; m_max = max; m_soft = soft; update(); }
    void setStep(double s) { m_step = s; }
    void setDecimals(int d) { m_decimals = d; update(); }
    void setSuffix(const QString &s) { m_suffix = s; update(); }
    void setValue(double v);
    double value() const { return m_value; }
    void setShowBar(bool on) { m_bar = on; update(); }
    QSize sizeHint() const override { return {70, 20}; }

signals:
    void valueChanged(double v);      // while dragging
    void editingFinished();           // drag released / text committed

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;

private:
    void edit();
    double clampV(double v) const;
    double m_min = 0, m_max = 100, m_step = 0.1, m_value = 0;
    bool m_soft = true, m_bar = false, m_dragged = false;
    int m_decimals = 1;
    QString m_suffix;
    QPoint m_press;
    double m_pressValue = 0;
};

class ColorButton : public QWidget {
    Q_OBJECT
public:
    explicit ColorButton(QWidget *parent = nullptr);
    void setColor(const QColor &c) { m_color = c; update(); }
    QColor color() const { return m_color; }
    QSize sizeHint() const override { return {40, 20}; }
signals:
    void colorChanged(const QColor &c);
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
private:
    QColor m_color = Qt::white;
};

// Curve editor. RGB mode edits {"master","r","g","b"} point lists; hue mode a single
// periodic curve (neutral at 0.5).
class CurveEditor : public QWidget {
    Q_OBJECT
public:
    enum Mode { Rgb, Hue };
    explicit CurveEditor(Mode mode, QWidget *parent = nullptr);
    void setJson(const QString &json);
    QString json() const;
    void setChannel(int c) { m_channel = c; update(); }   // rgb: 0 master, 1 r, 2 g, 3 b
    QSize sizeHint() const override { return {240, 200}; }
    QSize minimumSizeHint() const override { return {160, 120}; }
signals:
    void changed();
    void editingFinished();
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
private:
    using Pts = std::vector<std::pair<double, double>>;
    Pts &pts() { return m_pts[m_mode == Hue ? 0 : m_channel]; }
    QPointF toWidget(double x, double y) const;
    std::pair<double, double> fromWidget(QPointF p) const;
    Mode m_mode;
    int m_channel = 0;
    Pts m_pts[4];
    int m_drag = -1;
};

// Colour wheel: offset point in the unit disc.
class ColorWheel : public QWidget {
    Q_OBJECT
public:
    explicit ColorWheel(QWidget *parent = nullptr);
    void setPoint(QPointF p) { m_p = p; update(); }
    QPointF point() const { return m_p; }
    QSize sizeHint() const override { return {130, 130}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return w; }
signals:
    void pointChanged(QPointF p);
    void editingFinished();
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
private:
    void setFrom(QPoint pos);
    QPointF m_p;
    QImage m_wheel;
};

// Collapsible section header used in Effect Controls and Lumetri panels.
class Section : public QWidget {
    Q_OBJECT
public:
    Section(const QString &title, QWidget *content, QWidget *parent = nullptr, bool open = true);
    void setOpen(bool on);
    bool isOpen() const { return m_open; }
    QWidget *headerWidget() { return m_header; }
signals:
    void toggled(bool open);
private:
    QWidget *m_header;
    QWidget *m_content;
    bool m_open = true;
};

} // namespace rc
