#include "Lut.h"
#include <QFile>
#include <QTextStream>
#include <algorithm>
#include <cmath>

namespace rc {

bool loadCubeLut(const QString &path, int &size, std::vector<float> &rgb, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = f.errorString();
        return false;
    }
    int size3 = 0, size1 = 0;
    float dmin[3] = {0, 0, 0}, dmax[3] = {1, 1, 1};
    std::vector<float> data;
    QTextStream in(&f);
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        if (line.startsWith("TITLE"))
            continue;
        if (line.startsWith("LUT_3D_SIZE")) { size3 = line.section(' ', -1).toInt(); continue; }
        if (line.startsWith("LUT_1D_SIZE")) { size1 = line.section(' ', -1).toInt(); continue; }
        if (line.startsWith("DOMAIN_MIN")) {
            auto p = line.split(' ', Qt::SkipEmptyParts);
            for (int i = 0; i < 3 && i + 1 < p.size(); ++i) dmin[i] = p[i + 1].toFloat();
            continue;
        }
        if (line.startsWith("DOMAIN_MAX")) {
            auto p = line.split(' ', Qt::SkipEmptyParts);
            for (int i = 0; i < 3 && i + 1 < p.size(); ++i) dmax[i] = p[i + 1].toFloat();
            continue;
        }
        if (line.startsWith("LUT_") || line.at(0).isLetter())
            continue;
        auto p = line.split(QChar(' '), Qt::SkipEmptyParts);
        if (p.size() < 3)
            continue;
        for (int i = 0; i < 3; ++i)
            data.push_back(p[i].toFloat());
    }
    if (size3 >= 2 && data.size() >= size_t(size3) * size3 * size3 * 3) {
        size = size3;
        rgb.assign(data.begin(), data.begin() + size_t(size3) * size3 * size3 * 3);
        for (size_t i = 0; i < rgb.size(); ++i) {
            int c = int(i % 3);
            rgb[i] = (rgb[i] - dmin[c]) / std::max(1e-6f, dmax[c] - dmin[c]);
        }
        return true;
    }
    if (size1 >= 2 && data.size() >= size_t(size1) * 3) {
        size = 33;
        rgb.resize(size_t(size) * size * size * 3);
        auto lookup = [&](int ch, float v) {
            float x = std::clamp((v - dmin[ch]) / std::max(1e-6f, dmax[ch] - dmin[ch]), 0.f, 1.f) * (size1 - 1);
            int i0 = int(x);
            int i1 = std::min(size1 - 1, i0 + 1);
            float fr = x - i0;
            return data[size_t(i0) * 3 + ch] * (1 - fr) + data[size_t(i1) * 3 + ch] * fr;
        };
        for (int b = 0; b < size; ++b)
            for (int g = 0; g < size; ++g)
                for (int r = 0; r < size; ++r) {
                    size_t o = (size_t(b) * size * size + size_t(g) * size + r) * 3;
                    rgb[o] = lookup(0, float(r) / (size - 1));
                    rgb[o + 1] = lookup(1, float(g) / (size - 1));
                    rgb[o + 2] = lookup(2, float(b) / (size - 1));
                }
        return true;
    }
    if (error) *error = "Invalid or unsupported .cube file";
    return false;
}

std::vector<float> evalCurve(const std::vector<std::pair<double, double>> &ptsIn, int n, bool periodic, double neutral)
{
    std::vector<float> out(n);
    std::vector<std::pair<double, double>> pts = ptsIn;
    std::sort(pts.begin(), pts.end());
    if (pts.empty()) {
        for (int i = 0; i < n; ++i)
            out[i] = neutral >= 0 ? float(neutral) : float(i) / (n - 1);
        return out;
    }
    if (periodic) {
        auto first = pts.front(), last = pts.back();
        pts.insert(pts.begin(), {last.first - 1.0, last.second});
        pts.push_back({first.first + 1.0, first.second});
    } else {
        if (pts.front().first > 0) pts.insert(pts.begin(), {0.0, pts.front().second});
        if (pts.back().first < 1) pts.push_back({1.0, pts.back().second});
    }
    const size_t k = pts.size();
    if (k == 1) {
        std::fill(out.begin(), out.end(), float(pts[0].second));
        return out;
    }
    // Fritsch-Carlson monotone cubic tangents.
    std::vector<double> d(k - 1), m(k);
    for (size_t i = 0; i + 1 < k; ++i) {
        double dx = pts[i + 1].first - pts[i].first;
        d[i] = dx > 0 ? (pts[i + 1].second - pts[i].second) / dx : 0;
    }
    m[0] = d[0];
    m[k - 1] = d[k - 2];
    for (size_t i = 1; i + 1 < k; ++i)
        m[i] = (d[i - 1] * d[i] <= 0) ? 0 : (d[i - 1] + d[i]) / 2;
    for (size_t i = 0; i + 1 < k; ++i) {
        if (d[i] == 0) { m[i] = m[i + 1] = 0; continue; }
        double a = m[i] / d[i], b = m[i + 1] / d[i];
        double s = a * a + b * b;
        if (s > 9) {
            double t = 3 / std::sqrt(s);
            m[i] = t * a * d[i];
            m[i + 1] = t * b * d[i];
        }
    }
    for (int j = 0; j < n; ++j) {
        double x = double(j) / (n - 1);
        size_t i = 0;
        while (i + 2 < k && x > pts[i + 1].first)
            ++i;
        double x0 = pts[i].first, x1 = pts[i + 1].first;
        double h = x1 - x0;
        double t = h > 0 ? std::clamp((x - x0) / h, 0.0, 1.0) : 0;
        double t2 = t * t, t3 = t2 * t;
        double y = (2 * t3 - 3 * t2 + 1) * pts[i].second + (t3 - 2 * t2 + t) * h * m[i] + (-2 * t3 + 3 * t2) * pts[i + 1].second +
                   (t3 - t2) * h * m[i + 1];
        out[j] = float(std::clamp(y, 0.0, periodic || neutral >= 0 ? 1.0 : 1.0));
    }
    return out;
}

} // namespace rc
