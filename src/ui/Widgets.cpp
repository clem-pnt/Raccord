#include "Widgets.h"
#include "Icons.h"
#include "render/Lut.h"
#include <QColorDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

namespace rc {

// ---------------------------------------------------------------------------
ValueSlider::ValueSlider(QWidget *parent) : QWidget(parent)
{
    setCursor(Qt::SizeHorCursor);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumWidth(50);
}

double ValueSlider::clampV(double v) const
{
    if (m_soft)
        return v;
    return std::clamp(v, m_min, m_max);
}

void ValueSlider::setValue(double v)
{
    if (v == m_value)
        return;
    m_value = v;
    update();
}

void ValueSlider::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    if (m_bar && m_max > m_min) {
        double f = std::clamp((m_value - m_min) / (m_max - m_min), 0.0, 1.0);
        p.fillRect(QRect(0, height() - 3, width(), 3), QColor(50, 50, 56));
        p.fillRect(QRect(0, height() - 3, int(width() * f), 3), accentColor().darker(130));
    }
    p.setPen(hasFocus() ? accentColor().lighter(130) : accentColor());
    QFont f = font();
    f.setUnderline(true);
    p.setFont(f);
    p.drawText(rect().adjusted(2, 0, 0, -2), Qt::AlignVCenter | Qt::AlignLeft, QString::number(m_value, 'f', m_decimals) + m_suffix);
}

void ValueSlider::mousePressEvent(QMouseEvent *e)
{
    m_press = e->pos();
    m_pressValue = m_value;
    m_dragged = false;
}

void ValueSlider::mouseMoveEvent(QMouseEvent *e)
{
    if (!(e->buttons() & Qt::LeftButton))
        return;
    const int dx = e->pos().x() - m_press.x();
    if (std::abs(dx) < 2 && !m_dragged)
        return;
    m_dragged = true;
    double mul = (e->modifiers() & Qt::ShiftModifier) ? 10 : (e->modifiers() & Qt::ControlModifier) ? 0.1 : 1;
    double v = clampV(m_pressValue + dx * m_step * mul);
    if (v != m_value) {
        m_value = v;
        update();
        emit valueChanged(v);
    }
}

void ValueSlider::mouseReleaseEvent(QMouseEvent *)
{
    if (m_dragged)
        emit editingFinished();
    else
        edit();
    m_dragged = false;
}

void ValueSlider::mouseDoubleClickEvent(QMouseEvent *) {}

void ValueSlider::wheelEvent(QWheelEvent *e)
{
    if (!hasFocus()) {
        e->ignore();
        return;
    }
    double v = clampV(m_value + (e->angleDelta().y() > 0 ? m_step : -m_step) * 10);
    m_value = v;
    update();
    emit valueChanged(v);
    emit editingFinished();
}

void ValueSlider::edit()
{
    bool ok = false;
    double v = QInputDialog::getDouble(this, tr("Value"), tr("Value:"), m_value, m_soft ? -1e9 : m_min, m_soft ? 1e9 : m_max, m_decimals, &ok);
    if (ok) {
        m_value = v;
        update();
        emit valueChanged(v);
        emit editingFinished();
    }
}

// ---------------------------------------------------------------------------
ColorButton::ColorButton(QWidget *parent) : QWidget(parent) { setCursor(Qt::PointingHandCursor); }

void ColorButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    QRect r = rect().adjusted(1, 2, -1, -2);
    p.fillRect(r, QBrush(QColor(90, 90, 90), Qt::Dense4Pattern));
    p.fillRect(r, m_color);
    p.setPen(QColor(20, 20, 20));
    p.drawRect(r);
}

void ColorButton::mousePressEvent(QMouseEvent *)
{
    QColor c = QColorDialog::getColor(m_color, this, tr("Choose Color"), QColorDialog::ShowAlphaChannel);
    if (c.isValid()) {
        m_color = c;
        update();
        emit colorChanged(c);
    }
}

// ---------------------------------------------------------------------------
CurveEditor::CurveEditor(Mode mode, QWidget *parent) : QWidget(parent), m_mode(mode)
{
    setJson({});
    setMouseTracking(true);
}

static std::vector<std::pair<double, double>> readPts(const QJsonValue &v)
{
    std::vector<std::pair<double, double>> pts;
    for (const auto &p : v.toArray()) {
        QJsonArray a = p.toArray();
        if (a.size() >= 2)
            pts.push_back({a[0].toDouble(), a[1].toDouble()});
    }
    return pts;
}

static QJsonArray writePts(const std::vector<std::pair<double, double>> &pts)
{
    QJsonArray a;
    for (const auto &p : pts)
        a.append(QJsonArray{p.first, p.second});
    return a;
}

void CurveEditor::setJson(const QString &json)
{
    QJsonDocument d = QJsonDocument::fromJson(json.toUtf8());
    if (m_mode == Hue) {
        m_pts[0] = readPts(d.isArray() ? QJsonValue(d.array()) : d.object()["points"]);
    } else {
        const char *names[4] = {"master", "r", "g", "b"};
        for (int i = 0; i < 4; ++i) {
            m_pts[i] = readPts(d.object()[names[i]]);
            if (m_pts[i].size() < 2)
                m_pts[i] = {{0, 0}, {1, 1}};
        }
    }
    update();
}

QString CurveEditor::json() const
{
    if (m_mode == Hue) {
        if (m_pts[0].empty())
            return {};
        return QString::fromUtf8(QJsonDocument(writePts(m_pts[0])).toJson(QJsonDocument::Compact));
    }
    bool identity = true;
    for (int i = 0; i < 4; ++i)
        if (!(m_pts[i].size() == 2 && m_pts[i][0] == std::pair(0.0, 0.0) && m_pts[i][1] == std::pair(1.0, 1.0)))
            identity = false;
    if (identity)
        return {};
    QJsonObject o;
    const char *names[4] = {"master", "r", "g", "b"};
    for (int i = 0; i < 4; ++i)
        o[names[i]] = writePts(m_pts[i]);
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QPointF CurveEditor::toWidget(double x, double y) const
{
    QRectF r = QRectF(rect()).adjusted(6, 6, -6, -6);
    return {r.left() + x * r.width(), r.bottom() - y * r.height()};
}

std::pair<double, double> CurveEditor::fromWidget(QPointF p) const
{
    QRectF r = QRectF(rect()).adjusted(6, 6, -6, -6);
    return {std::clamp((p.x() - r.left()) / r.width(), 0.0, 1.0), std::clamp((r.bottom() - p.y()) / r.height(), 0.0, 1.0)};
}

void CurveEditor::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QRectF r = QRectF(rect()).adjusted(6, 6, -6, -6);
    if (m_mode == Hue) {
        QLinearGradient g(r.topLeft(), r.topRight());
        for (int i = 0; i <= 6; ++i)
            g.setColorAt(i / 6.0, QColor::fromHsvF(std::fmod(i / 6.0, 1.0), 0.6, 0.35));
        p.fillRect(r, g);
    } else {
        p.fillRect(r, QColor(24, 24, 28));
    }
    p.setPen(QColor(60, 60, 66));
    for (int i = 1; i < 4; ++i) {
        p.drawLine(QPointF(r.left() + r.width() * i / 4, r.top()), QPointF(r.left() + r.width() * i / 4, r.bottom()));
        p.drawLine(QPointF(r.left(), r.top() + r.height() * i / 4), QPointF(r.right(), r.top() + r.height() * i / 4));
    }
    if (m_mode == Hue) {
        p.setPen(QPen(QColor(255, 255, 255, 80), 1, Qt::DashLine));
        p.drawLine(toWidget(0, 0.5), toWidget(1, 0.5));
    }
    const QColor colors[4] = {QColor(230, 230, 230), QColor(240, 80, 80), QColor(80, 220, 80), QColor(90, 140, 255)};
    const int n = 256;
    auto drawCurve = [&](const Pts &pts, QColor c, bool active) {
        std::vector<float> v = m_mode == Hue ? evalCurve(pts, n, true, 0.5) : evalCurve(pts, n);
        QPainterPath path;
        for (int i = 0; i < n; ++i) {
            QPointF q = toWidget(i / double(n - 1), v[i]);
            if (i == 0) path.moveTo(q); else path.lineTo(q);
        }
        p.setPen(QPen(active ? c : QColor(c.red(), c.green(), c.blue(), 70), active ? 1.6 : 1));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    };
    if (m_mode == Rgb) {
        for (int i = 0; i < 4; ++i)
            if (i != m_channel) drawCurve(m_pts[i], colors[i], false);
        drawCurve(m_pts[m_channel], colors[m_channel], true);
    } else {
        drawCurve(m_pts[0], Qt::white, true);
    }
    p.setBrush(Qt::white);
    p.setPen(Qt::black);
    const Pts &cur = m_mode == Hue ? m_pts[0] : m_pts[m_channel];
    for (const auto &pt : cur)
        p.drawEllipse(toWidget(pt.first, pt.second), 4, 4);
}

void CurveEditor::mousePressEvent(QMouseEvent *e)
{
    Pts &v = pts();
    m_drag = -1;
    for (int i = 0; i < (int)v.size(); ++i)
        if (QLineF(toWidget(v[i].first, v[i].second), e->position()).length() < 7)
            m_drag = i;
    if (e->button() == Qt::RightButton || (e->modifiers() & Qt::ControlModifier)) {
        // Delete point (keep the end points in RGB mode).
        if (m_drag >= 0 && (m_mode == Hue || (m_drag != 0 && m_drag != (int)v.size() - 1))) {
            v.erase(v.begin() + m_drag);
            emit changed();
            emit editingFinished();
            update();
        }
        m_drag = -1;
        return;
    }
    if (m_drag < 0) {
        auto pt = fromWidget(e->position());
        if (m_mode == Hue && v.empty()) {
            // Seed with neutral anchors so a drag bends locally.
            v = {{std::fmod(pt.first + 0.85, 1.0), 0.5}, {std::fmod(pt.first + 0.15, 1.0), 0.5}};
        }
        v.push_back(pt);
        std::sort(v.begin(), v.end());
        for (int i = 0; i < (int)v.size(); ++i)
            if (v[i] == pt) m_drag = i;
        emit changed();
        update();
    }
}

void CurveEditor::mouseMoveEvent(QMouseEvent *e)
{
    if (m_drag < 0 || !(e->buttons() & Qt::LeftButton))
        return;
    Pts &v = pts();
    auto pt = fromWidget(e->position());
    if (m_mode == Rgb) {
        if (m_drag == 0) pt.first = std::min(pt.first, v.size() > 1 ? v[1].first - 0.01 : 1.0);
        else if (m_drag == (int)v.size() - 1) pt.first = std::max(pt.first, v[m_drag - 1].first + 0.01);
        else pt.first = std::clamp(pt.first, v[m_drag - 1].first + 0.01, v[m_drag + 1].first - 0.01);
    }
    v[m_drag] = pt;
    if (m_mode == Hue) {
        auto moved = pt;
        std::sort(v.begin(), v.end());
        for (int i = 0; i < (int)v.size(); ++i)
            if (v[i] == moved) m_drag = i;
    }
    emit changed();
    update();
}

void CurveEditor::mouseReleaseEvent(QMouseEvent *)
{
    if (m_drag >= 0)
        emit editingFinished();
    m_drag = -1;
}

void CurveEditor::mouseDoubleClickEvent(QMouseEvent *)
{
    // Reset the current channel.
    if (m_mode == Hue) m_pts[0].clear();
    else m_pts[m_channel] = {{0, 0}, {1, 1}};
    emit changed();
    emit editingFinished();
    update();
}

// ---------------------------------------------------------------------------
ColorWheel::ColorWheel(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(90, 90);
    QSizePolicy sp(QSizePolicy::Preferred, QSizePolicy::Preferred);
    sp.setHeightForWidth(true);
    setSizePolicy(sp);
}

void ColorWheel::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const int s = std::min(width(), height()) - 4;
    QRect r((width() - s) / 2, (height() - s) / 2, s, s);
    if (m_wheel.width() != s) {
        m_wheel = QImage(s, s, QImage::Format_ARGB32_Premultiplied);
        m_wheel.fill(Qt::transparent);
        for (int y = 0; y < s; ++y)
            for (int x = 0; x < s; ++x) {
                double dx = (x - s / 2.0) / (s / 2.0), dy = (y - s / 2.0) / (s / 2.0);
                double d = std::hypot(dx, dy);
                if (d > 1)
                    continue;
                double h = std::fmod(std::atan2(-dy, dx) / (2 * M_PI) + 1.0, 1.0);
                QColor c = QColor::fromHsvF(h, std::min(1.0, d) * 0.75, 0.55);
                double a = std::clamp((1 - d) * s / 2, 0.0, 1.0);
                c.setAlphaF(a);
                m_wheel.setPixelColor(x, y, c);
            }
    }
    p.drawImage(r, m_wheel);
    p.setPen(QColor(255, 255, 255, 60));
    p.drawLine(r.center().x(), r.top() + 4, r.center().x(), r.bottom() - 4);
    p.drawLine(r.left() + 4, r.center().y(), r.right() - 4, r.center().y());
    QPointF c(r.center().x() + m_p.x() * s / 2, r.center().y() + m_p.y() * s / 2);
    p.setPen(QPen(Qt::white, 2));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(c, 5, 5);
}

void ColorWheel::setFrom(QPoint pos)
{
    const int s = std::min(width(), height()) - 4;
    QPointF d = (QPointF(pos) - QPointF(width() / 2.0, height() / 2.0)) / (s / 2.0);
    double len = std::hypot(d.x(), d.y());
    if (len > 1)
        d /= len;
    m_p = d;
    update();
    emit pointChanged(m_p);
}

void ColorWheel::mousePressEvent(QMouseEvent *e) { setFrom(e->pos()); }
void ColorWheel::mouseMoveEvent(QMouseEvent *e) { if (e->buttons() & Qt::LeftButton) setFrom(e->pos()); }
void ColorWheel::mouseReleaseEvent(QMouseEvent *) { emit editingFinished(); }
void ColorWheel::mouseDoubleClickEvent(QMouseEvent *)
{
    m_p = {};
    update();
    emit pointChanged(m_p);
    emit editingFinished();
}

// ---------------------------------------------------------------------------
Section::Section(const QString &title, QWidget *content, QWidget *parent, bool open) : QWidget(parent), m_content(content)
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    auto *hdr = new QWidget(this);
    hdr->setObjectName("SectionHeader");
    hdr->setStyleSheet("#SectionHeader { background: #2b2b31; border-top: 1px solid #3a3a42; }");
    auto *hl = new QHBoxLayout(hdr);
    hl->setContentsMargins(4, 2, 4, 2);
    auto *btn = new QToolButton(hdr);
    btn->setAutoRaise(true);
    btn->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    btn->setText(title);
    btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    btn->setStyleSheet("QToolButton { font-weight: bold; border: none; }");
    hl->addWidget(btn);
    hl->addStretch();
    lay->addWidget(hdr);
    lay->addWidget(content);
    m_header = hdr;
    connect(btn, &QToolButton::clicked, this, [this, btn] {
        setOpen(!m_open);
        btn->setArrowType(m_open ? Qt::DownArrow : Qt::RightArrow);
    });
    setOpen(open);
}

void Section::setOpen(bool on)
{
    m_open = on;
    m_content->setVisible(on);
    emit toggled(on);
}

} // namespace rc
