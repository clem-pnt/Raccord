#include "Scopes.h"
#include <QComboBox>
#include <QPainter>
#include <QVBoxLayout>
#include <cmath>
#include <vector>

namespace rc {

ScopeView::ScopeView(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(200, 150);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void ScopeView::setFrame(const QImage &img)
{
    if (!isVisible())
        return;
    // Work on a small copy: scopes need distribution, not detail.
    m_frame = img.width() > 480 ? img.scaledToWidth(480, Qt::FastTransformation).convertToFormat(QImage::Format_RGB32)
                                : img.convertToFormat(QImage::Format_RGB32);
    recompute();
}

static inline float luma709(int r, int g, int b) { return (0.2126f * r + 0.7152f * g + 0.0722f * b) / 255.f; }

void ScopeView::recompute()
{
    const int W = std::max(1, width()), H = std::max(1, height());
    m_plot = QImage(W, H, QImage::Format_RGB32);
    m_plot.fill(QColor(14, 14, 16));
    if (m_frame.isNull()) {
        update();
        return;
    }
    const int fw = m_frame.width(), fh = m_frame.height();
    std::vector<float> acc[3];
    for (auto &a : acc) a.assign(size_t(W) * H, 0.f);
    auto plotAdd = [&](int ch, int x, int y, float v) {
        if (x < 0 || y < 0 || x >= W || y >= H) return;
        acc[ch][size_t(y) * W + x] += v;
    };
    const float density = 40.0f * W * H / float(fw * fh) / 255.f;
    if (m_mode == Waveform || m_mode == Parade) {
        const int cols = m_mode == Parade ? 3 : 1;
        const int colW = W / cols;
        for (int y = 0; y < fh; ++y) {
            const QRgb *line = reinterpret_cast<const QRgb *>(m_frame.constScanLine(y));
            for (int x = 0; x < fw; ++x) {
                const int r = qRed(line[x]), g = qGreen(line[x]), b = qBlue(line[x]);
                if (m_mode == Waveform) {
                    const int px = x * W / fw;
                    const int py = int((1.0f - luma709(r, g, b)) * (H - 1));
                    plotAdd(0, px, py, density); plotAdd(1, px, py, density); plotAdd(2, px, py, density);
                } else {
                    const int v[3] = {r, g, b};
                    for (int c = 0; c < 3; ++c)
                        plotAdd(c, c * colW + x * colW / fw, int((1.0f - v[c] / 255.f) * (H - 1)), density * 1.5f);
                }
            }
        }
    } else if (m_mode == Vectorscope) {
        const int S = std::min(W, H) - 10;
        const int cx = W / 2, cy = H / 2;
        for (int y = 0; y < fh; ++y) {
            const QRgb *line = reinterpret_cast<const QRgb *>(m_frame.constScanLine(y));
            for (int x = 0; x < fw; ++x) {
                const float r = qRed(line[x]) / 255.f, g = qGreen(line[x]) / 255.f, b = qBlue(line[x]) / 255.f;
                const float Y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                const float cb = (b - Y) / 1.8556f, cr = (r - Y) / 1.5748f;
                const int px = cx + int(cb * S), py = cy - int(cr * S);
                // Tint the trace with the source colour.
                plotAdd(0, px, py, density * (0.4f + r));
                plotAdd(1, px, py, density * (0.4f + g));
                plotAdd(2, px, py, density * (0.4f + b));
            }
        }
    } else {
        std::vector<int> hist[3];
        for (auto &h : hist) h.assign(256, 0);
        for (int y = 0; y < fh; ++y) {
            const QRgb *line = reinterpret_cast<const QRgb *>(m_frame.constScanLine(y));
            for (int x = 0; x < fw; ++x) {
                hist[0][qRed(line[x])]++;
                hist[1][qGreen(line[x])]++;
                hist[2][qBlue(line[x])]++;
            }
        }
        int mx = 1;
        for (auto &h : hist) for (int i = 1; i < 255; ++i) mx = std::max(mx, h[i]);
        for (int c = 0; c < 3; ++c)
            for (int x = 0; x < W; ++x) {
                const int bin = x * 256 / W;
                const int top = H - 1 - int(std::min(1.0, double(hist[c][bin]) / mx) * (H - 4));
                for (int y = top; y < H; ++y) acc[c][size_t(y) * W + x] = 0.5f;
            }
    }
    for (int y = 0; y < H; ++y) {
        QRgb *out = reinterpret_cast<QRgb *>(m_plot.scanLine(y));
        for (int x = 0; x < W; ++x) {
            const size_t i = size_t(y) * W + x;
            float r = acc[0][i], g = acc[1][i], b = acc[2][i];
            if (r == 0 && g == 0 && b == 0) continue;
            if (m_mode == Parade) {
                const int col = x * 3 / W;
                // Each third shows a single channel.
                const float v = col == 0 ? r : col == 1 ? g : b;
                r = col == 0 ? v : v * 0.25f;
                g = col == 1 ? v : v * 0.25f;
                b = col == 2 ? v : v * 0.25f;
            } else if (m_mode == Waveform) {
                r *= 0.55f; b *= 0.6f;   // greenish trace
            }
            auto tone = [](float v) { return int(255 * (1.0f - std::exp(-v * 1.6f))); };
            out[x] = qRgb(std::min(255, 14 + tone(r)), std::min(255, 14 + tone(g)), std::min(255, 16 + tone(b)));
        }
    }
    update();
}

void ScopeView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.drawImage(0, 0, m_plot);
    p.setPen(QColor(255, 255, 255, 50));
    QFont f = font();
    f.setPixelSize(9);
    p.setFont(f);
    const int W = width(), H = height();
    if (m_mode == Waveform || m_mode == Parade) {
        for (int v = 0; v <= 100; v += 10) {
            const int y = int((1.0 - v / 100.0) * (H - 1));
            p.drawLine(0, y, W, y);
            p.drawText(2, y - 1, QString::number(v));
        }
        if (m_mode == Parade)
            for (int c = 1; c < 3; ++c) p.drawLine(c * W / 3, 0, c * W / 3, H);
    } else if (m_mode == Vectorscope) {
        const int S = std::min(W, H) - 10;
        const QPoint c(W / 2, H / 2);
        p.drawEllipse(c, int(S * 0.5), int(S * 0.5));
        p.drawLine(c.x() - S / 2, c.y(), c.x() + S / 2, c.y());
        p.drawLine(c.x(), c.y() - S / 2, c.x(), c.y() + S / 2);
        // 75% colour targets.
        const struct { const char *n; float r, g, b; } targets[] = {{"R", .75f, 0, 0}, {"Mg", .75f, 0, .75f}, {"B", 0, 0, .75f},
                                                                    {"Cy", 0, .75f, .75f}, {"G", 0, .75f, 0}, {"Yl", .75f, .75f, 0}};
        for (const auto &t : targets) {
            const float Y = 0.2126f * t.r + 0.7152f * t.g + 0.0722f * t.b;
            const float cb = (t.b - Y) / 1.8556f, cr = (t.r - Y) / 1.5748f;
            const QPoint pt(c.x() + int(cb * S), c.y() - int(cr * S));
            p.drawRect(pt.x() - 4, pt.y() - 4, 8, 8);
            p.drawText(pt + QPoint(6, -4), t.n);
        }
        // Skin tone line (~123 degrees).
        const double a = 123.0 * M_PI / 180.0;
        p.setPen(QColor(255, 200, 150, 70));
        p.drawLine(c, c + QPoint(int(std::cos(a) * S / 2), -int(std::sin(a) * S / 2)));
    }
}

ScopesPanel::ScopesPanel(QWidget *parent) : QWidget(parent)
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(2, 2, 2, 2);
    m_mode = new QComboBox(this);
    m_mode->addItems({tr("Waveform (Luma)"), tr("RGB Parade"), tr("Vectorscope YUV"), tr("Histogram")});
    lay->addWidget(m_mode);
    m_view = new ScopeView(this);
    lay->addWidget(m_view, 1);
    connect(m_mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { m_view->setMode(ScopeView::Mode(i)); });
}

} // namespace rc
