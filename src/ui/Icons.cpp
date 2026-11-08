#include "Icons.h"
#include <QApplication>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QStyleFactory>

namespace rc {

QColor accentColor() { return QColor(74, 144, 255); }

namespace {

void drawIcon(QPainter &p, const QString &n, const QRectF &r, const QColor &c)
{
    p.setRenderHint(QPainter::Antialiasing);
    const double s = r.width();
    auto P = [&](double x, double y) { return QPointF(r.x() + x * s, r.y() + y * s); };
    QPen pen(c, std::max(1.2, s * 0.09), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    auto poly = [&](std::initializer_list<QPointF> pts, bool fill = true) {
        QPolygonF pg(pts);
        if (fill) { p.setBrush(c); p.setPen(Qt::NoPen); }
        p.drawPolygon(pg);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
    };
    if (n == "play") poly({P(0.3, 0.2), P(0.8, 0.5), P(0.3, 0.8)});
    else if (n == "pause") { p.fillRect(QRectF(P(0.28, 0.2), P(0.43, 0.8)), c); p.fillRect(QRectF(P(0.57, 0.2), P(0.72, 0.8)), c); }
    else if (n == "step-forward") { poly({P(0.25, 0.25), P(0.6, 0.5), P(0.25, 0.75)}); p.drawLine(P(0.7, 0.25), P(0.7, 0.75)); }
    else if (n == "step-back") { poly({P(0.75, 0.25), P(0.4, 0.5), P(0.75, 0.75)}); p.drawLine(P(0.3, 0.25), P(0.3, 0.75)); }
    else if (n == "go-in") { poly({P(0.75, 0.25), P(0.45, 0.5), P(0.75, 0.75)}); p.drawLine(P(0.3, 0.2), P(0.3, 0.8)); p.drawLine(P(0.3, 0.2), P(0.4, 0.2)); p.drawLine(P(0.3, 0.8), P(0.4, 0.8)); }
    else if (n == "go-out") { poly({P(0.25, 0.25), P(0.55, 0.5), P(0.25, 0.75)}); p.drawLine(P(0.7, 0.2), P(0.7, 0.8)); p.drawLine(P(0.7, 0.2), P(0.6, 0.2)); p.drawLine(P(0.7, 0.8), P(0.6, 0.8)); }
    else if (n == "mark-in") { p.drawLine(P(0.4, 0.2), P(0.4, 0.8)); p.drawLine(P(0.4, 0.2), P(0.65, 0.2)); p.drawLine(P(0.4, 0.8), P(0.65, 0.8)); }
    else if (n == "mark-out") { p.drawLine(P(0.6, 0.2), P(0.6, 0.8)); p.drawLine(P(0.6, 0.2), P(0.35, 0.2)); p.drawLine(P(0.6, 0.8), P(0.35, 0.8)); }
    else if (n == "loop") { p.drawArc(QRectF(P(0.2, 0.25), P(0.8, 0.75)), 30 * 16, 300 * 16); poly({P(0.72, 0.18), P(0.85, 0.35), P(0.65, 0.38)}); }
    else if (n == "insert") { p.drawRect(QRectF(P(0.15, 0.55), P(0.85, 0.8))); poly({P(0.38, 0.15), P(0.62, 0.15), P(0.62, 0.32), P(0.72, 0.32), P(0.5, 0.5), P(0.28, 0.32), P(0.38, 0.32)}); }
    else if (n == "overwrite") { p.fillRect(QRectF(P(0.15, 0.55), P(0.85, 0.8)), c); poly({P(0.38, 0.15), P(0.62, 0.15), P(0.62, 0.32), P(0.72, 0.32), P(0.5, 0.5), P(0.28, 0.32), P(0.38, 0.32)}); }
    else if (n == "camera") { p.drawRoundedRect(QRectF(P(0.15, 0.3), P(0.85, 0.78)), s * 0.06, s * 0.06); p.drawEllipse(P(0.5, 0.54), s * 0.14, s * 0.14); p.drawLine(P(0.38, 0.3), P(0.44, 0.2)); p.drawLine(P(0.44, 0.2), P(0.56, 0.2)); p.drawLine(P(0.56, 0.2), P(0.62, 0.3)); }
    else if (n == "safe") { p.drawRect(QRectF(P(0.15, 0.2), P(0.85, 0.8))); p.drawRect(QRectF(P(0.3, 0.33), P(0.7, 0.67))); }
    else if (n == "select") poly({P(0.3, 0.15), P(0.72, 0.55), P(0.52, 0.57), P(0.63, 0.82), P(0.55, 0.86), P(0.44, 0.6), P(0.3, 0.72)});
    else if (n == "track-select") { poly({P(0.2, 0.3), P(0.45, 0.5), P(0.2, 0.7)}); poly({P(0.45, 0.3), P(0.7, 0.5), P(0.45, 0.7)}); p.drawLine(P(0.8, 0.2), P(0.8, 0.8)); }
    else if (n == "ripple") { p.drawLine(P(0.5, 0.15), P(0.5, 0.85)); poly({P(0.2, 0.35), P(0.42, 0.5), P(0.2, 0.65)}); p.drawLine(P(0.6, 0.35), P(0.8, 0.35)); p.drawLine(P(0.6, 0.65), P(0.8, 0.65)); }
    else if (n == "roll") { p.drawLine(P(0.5, 0.15), P(0.5, 0.85)); poly({P(0.15, 0.5), P(0.38, 0.32), P(0.38, 0.68)}); poly({P(0.85, 0.5), P(0.62, 0.32), P(0.62, 0.68)}); }
    else if (n == "rate") { p.drawLine(P(0.15, 0.5), P(0.85, 0.5)); poly({P(0.15, 0.5), P(0.32, 0.36), P(0.32, 0.64)}); poly({P(0.85, 0.5), P(0.68, 0.36), P(0.68, 0.64)}); p.drawEllipse(P(0.5, 0.5), s * 0.1, s * 0.1); }
    else if (n == "razor") { poly({P(0.2, 0.75), P(0.75, 0.2), P(0.82, 0.27), P(0.27, 0.82)}); p.drawLine(P(0.15, 0.85), P(0.3, 0.7)); }
    else if (n == "slip") { p.drawRect(QRectF(P(0.25, 0.3), P(0.75, 0.7))); p.drawLine(P(0.1, 0.5), P(0.9, 0.5)); }
    else if (n == "slide") { p.drawRect(QRectF(P(0.35, 0.3), P(0.65, 0.7))); poly({P(0.08, 0.5), P(0.25, 0.38), P(0.25, 0.62)}); poly({P(0.92, 0.5), P(0.75, 0.38), P(0.75, 0.62)}); }
    else if (n == "pen") { poly({P(0.5, 0.12), P(0.72, 0.55), P(0.5, 0.85), P(0.28, 0.55)}, false); p.drawLine(P(0.5, 0.12), P(0.5, 0.55)); }
    else if (n == "hand") { p.drawRoundedRect(QRectF(P(0.28, 0.35), P(0.72, 0.85)), s * 0.12, s * 0.12); for (double x : {0.33, 0.45, 0.57, 0.68}) p.drawLine(P(x, 0.38), P(x, 0.15)); }
    else if (n == "zoom") { p.drawEllipse(P(0.42, 0.42), s * 0.22, s * 0.22); p.drawLine(P(0.58, 0.58), P(0.85, 0.85)); }
    else if (n == "type") { p.drawLine(P(0.2, 0.2), P(0.8, 0.2)); p.drawLine(P(0.5, 0.2), P(0.5, 0.82)); }
    else if (n == "snap") { p.drawArc(QRectF(P(0.25, 0.2), P(0.75, 0.7)), 0, 180 * 16); p.drawLine(P(0.25, 0.45), P(0.25, 0.8)); p.drawLine(P(0.75, 0.45), P(0.75, 0.8)); }
    else if (n == "link") { p.drawRoundedRect(QRectF(P(0.12, 0.38), P(0.55, 0.62)), s * 0.12, s * 0.12); p.drawRoundedRect(QRectF(P(0.45, 0.38), P(0.88, 0.62)), s * 0.12, s * 0.12); }
    else if (n == "marker") poly({P(0.3, 0.15), P(0.7, 0.15), P(0.7, 0.6), P(0.5, 0.85), P(0.3, 0.6)});
    else if (n == "eye") { p.drawEllipse(QRectF(P(0.12, 0.32), P(0.88, 0.68))); p.setBrush(c); p.drawEllipse(P(0.5, 0.5), s * 0.1, s * 0.1); }
    else if (n == "lock") { p.drawRect(QRectF(P(0.25, 0.45), P(0.75, 0.82))); p.drawArc(QRectF(P(0.33, 0.18), P(0.67, 0.6)), 0, 180 * 16); }
    else if (n == "mute") { p.drawText(r, Qt::AlignCenter, "M"); }
    else if (n == "solo") { p.drawText(r, Qt::AlignCenter, "S"); }
    else if (n == "folder") { poly({P(0.12, 0.28), P(0.4, 0.28), P(0.48, 0.36), P(0.88, 0.36), P(0.88, 0.78), P(0.12, 0.78)}, false); }
    else if (n == "film") { p.drawRect(QRectF(P(0.15, 0.22), P(0.85, 0.78))); for (double y : {0.3, 0.5, 0.7}) { p.drawPoint(P(0.24, y)); p.drawPoint(P(0.76, y)); } }
    else if (n == "audio") { p.drawLine(P(0.3, 0.7), P(0.3, 0.3)); p.drawLine(P(0.3, 0.3), P(0.75, 0.2)); p.drawLine(P(0.75, 0.2), P(0.75, 0.6)); p.setBrush(c); p.drawEllipse(P(0.24, 0.72), s * 0.08, s * 0.07); p.drawEllipse(P(0.69, 0.62), s * 0.08, s * 0.07); }
    else if (n == "image") { p.drawRect(QRectF(P(0.15, 0.22), P(0.85, 0.78))); poly({P(0.2, 0.72), P(0.42, 0.45), P(0.58, 0.62), P(0.66, 0.53), P(0.8, 0.72)}); }
    else if (n == "sequence") { p.drawRect(QRectF(P(0.12, 0.25), P(0.88, 0.75))); p.drawLine(P(0.12, 0.42), P(0.88, 0.42)); p.drawLine(P(0.12, 0.58), P(0.88, 0.58)); p.drawLine(P(0.5, 0.25), P(0.5, 0.42)); }
    else if (n == "fx") { QFont f = p.font(); f.setPixelSize(int(s * 0.5)); f.setItalic(true); f.setBold(true); p.setFont(f); p.drawText(r, Qt::AlignCenter, "fx"); }
    else if (n == "keyframe") poly({P(0.5, 0.2), P(0.8, 0.5), P(0.5, 0.8), P(0.2, 0.5)});
    else if (n == "keyframe-off") poly({P(0.5, 0.2), P(0.8, 0.5), P(0.5, 0.8), P(0.2, 0.5)}, false);
    else if (n == "stopwatch") { p.drawEllipse(P(0.5, 0.56), s * 0.28, s * 0.28); p.drawLine(P(0.5, 0.56), P(0.5, 0.38)); p.drawLine(P(0.43, 0.18), P(0.57, 0.18)); }
    else if (n == "reset") { p.drawArc(QRectF(P(0.22, 0.22), P(0.78, 0.78)), 90 * 16, 270 * 16); poly({P(0.5, 0.1), P(0.66, 0.22), P(0.5, 0.34)}); }
    else if (n == "trash") { p.drawRect(QRectF(P(0.28, 0.3), P(0.72, 0.85))); p.drawLine(P(0.2, 0.3), P(0.8, 0.3)); p.drawLine(P(0.42, 0.18), P(0.58, 0.18)); }
    else if (n == "mask-ellipse") { p.setPen(QPen(c, std::max(1.0, s * 0.06), Qt::DashLine)); p.drawEllipse(QRectF(P(0.18, 0.25), P(0.82, 0.75))); }
    else if (n == "mask-rect") { p.setPen(QPen(c, std::max(1.0, s * 0.06), Qt::DashLine)); p.drawRect(QRectF(P(0.2, 0.25), P(0.8, 0.75))); }
    else if (n == "plus") { p.drawLine(P(0.5, 0.2), P(0.5, 0.8)); p.drawLine(P(0.2, 0.5), P(0.8, 0.5)); }
    else if (n == "wand") { p.drawLine(P(0.2, 0.8), P(0.65, 0.35)); for (auto pt : {P(0.75, 0.15), P(0.85, 0.3), P(0.62, 0.15)}) p.drawPoint(pt); poly({P(0.72, 0.2), P(0.8, 0.28), P(0.72, 0.36), P(0.64, 0.28)}); }
    else if (n == "caption") { p.drawRoundedRect(QRectF(P(0.12, 0.25), P(0.88, 0.75)), s * 0.08, s * 0.08); p.drawLine(P(0.25, 0.45), P(0.5, 0.45)); p.drawLine(P(0.55, 0.45), P(0.75, 0.45)); p.drawLine(P(0.25, 0.6), P(0.65, 0.6)); }
    else if (n == "export") { p.drawLine(P(0.5, 0.15), P(0.5, 0.6)); poly({P(0.3, 0.38), P(0.5, 0.15), P(0.7, 0.38)}); p.drawLine(P(0.2, 0.6), P(0.2, 0.82)); p.drawLine(P(0.2, 0.82), P(0.8, 0.82)); p.drawLine(P(0.8, 0.82), P(0.8, 0.6)); }
    else if (n == "import") { p.drawLine(P(0.5, 0.15), P(0.5, 0.6)); poly({P(0.3, 0.4), P(0.5, 0.63), P(0.7, 0.4)}); p.drawLine(P(0.2, 0.6), P(0.2, 0.82)); p.drawLine(P(0.2, 0.82), P(0.8, 0.82)); p.drawLine(P(0.8, 0.82), P(0.8, 0.6)); }
    else if (n == "raccord") {
        p.setPen(Qt::NoPen);
        p.setBrush(accentColor());
        p.drawRoundedRect(r.adjusted(s * 0.05, s * 0.05, -s * 0.05, -s * 0.05), s * 0.22, s * 0.22);
        p.setBrush(Qt::white);
        p.drawPolygon(QPolygonF({P(0.25, 0.25), P(0.52, 0.25), P(0.52, 0.75), P(0.25, 0.75)}));
        p.setBrush(QColor(255, 255, 255, 170));
        p.drawPolygon(QPolygonF({P(0.56, 0.25), P(0.78, 0.5), P(0.56, 0.75)}));
    } else { p.drawEllipse(P(0.5, 0.5), s * 0.2, s * 0.2); }
}

class VecEngine : public QIconEngine {
public:
    explicit VecEngine(const QString &n) : m_name(n) {}
    void paint(QPainter *p, const QRect &rect, QIcon::Mode mode, QIcon::State state) override
    {
        QColor c = mode == QIcon::Disabled ? QColor(90, 90, 98) : (state == QIcon::On || mode == QIcon::Selected) ? accentColor() : QColor(205, 205, 212);
        drawIcon(*p, m_name, QRectF(rect), c);
    }
    QIconEngine *clone() const override { return new VecEngine(m_name); }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        QPixmap pm(size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        paint(&p, QRect(QPoint(0, 0), size), mode, state);
        return pm;
    }
private:
    QString m_name;
};

} // namespace

QIcon Icons::get(const QString &name)
{
    static QHash<QString, QIcon> cache;
    auto it = cache.find(name);
    if (it != cache.end())
        return *it;
    QIcon i(new VecEngine(name));
    cache.insert(name, i);
    return i;
}

void applyTheme(QApplication &app)
{
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    p.setColor(QPalette::Window, QColor(32, 32, 36));
    p.setColor(QPalette::WindowText, QColor(214, 214, 220));
    p.setColor(QPalette::Base, QColor(24, 24, 27));
    p.setColor(QPalette::AlternateBase, QColor(36, 36, 40));
    p.setColor(QPalette::ToolTipBase, QColor(46, 46, 52));
    p.setColor(QPalette::ToolTipText, QColor(230, 230, 235));
    p.setColor(QPalette::Text, QColor(214, 214, 220));
    p.setColor(QPalette::Button, QColor(44, 44, 50));
    p.setColor(QPalette::ButtonText, QColor(214, 214, 220));
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Highlight, accentColor());
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, accentColor());
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(110, 110, 118));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(110, 110, 118));
    app.setPalette(p);
    app.setStyleSheet(R"(
        QMainWindow::separator { background: #18181b; width: 4px; height: 4px; }
        QDockWidget { titlebar-close-icon: none; font-weight: 600; }
        QDockWidget::title { background: #26262b; padding: 4px 8px; border-bottom: 1px solid #18181b; }
        QTabBar::tab { background: #26262b; color: #a8a8b2; padding: 5px 12px; border: none; }
        QTabBar::tab:selected { color: #ffffff; border-bottom: 2px solid #4a90ff; background: #2c2c32; }
        QToolTip { border: 1px solid #44444c; padding: 4px; }
        QLabel#timecode { color: #4a90ff; font-family: monospace; font-size: 15px; font-weight: 600; }
        QLabel#timecodeDim { color: #8a8a96; font-family: monospace; font-size: 13px; }
        QLabel#monitorTitle { color: #a8a8b2; padding: 2px 4px; }
        QToolButton { padding: 3px; border-radius: 3px; }
        QToolButton:hover { background: #3a3a42; }
        QToolButton:checked { background: #2d3e5c; }
        QSlider::groove:horizontal { height: 4px; background: #3a3a42; border-radius: 2px; }
        QSlider::handle:horizontal { width: 12px; margin: -5px 0; background: #4a90ff; border-radius: 6px; }
        QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox { background: #1c1c20; border: 1px solid #3a3a42; border-radius: 3px; padding: 2px 4px; }
        QPushButton { background: #3a3a42; border: none; border-radius: 4px; padding: 5px 14px; }
        QPushButton:hover { background: #45454e; }
        QPushButton#primary { background: #4a90ff; color: white; font-weight: 600; }
        QHeaderView::section { background: #26262b; border: none; padding: 4px; color: #a8a8b2; }
        QTreeView, QListView, QTableView { border: none; }
        QMenu { background: #2a2a30; border: 1px solid #3a3a42; }
        QMenu::item:selected { background: #4a90ff; }
        QMenuBar::item:selected { background: #3a3a42; }
        QStatusBar { color: #8a8a96; }
    )");
}

} // namespace rc
