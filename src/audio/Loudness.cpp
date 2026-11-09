#include "Loudness.h"
#include <algorithm>
#include <cmath>

namespace rc {

LoudnessMeter::LoudnessMeter(int channels, int sampleRate) : m_ch(std::max(1, channels)), m_rate(std::max(8000, sampleRate))
{
    // Filter design as in BS.1770 (coefficients recomputed for any rate).
    double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
    double K = std::tan(M_PI * f0 / m_rate);
    const double Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
    double pb[3], pa[3] = {1, 0, 0}, rb[3] = {1, -2, 1}, ra[3] = {1, 0, 0};
    double a0 = 1.0 + K / Q + K * K;
    pb[0] = (Vh + Vb * K / Q + K * K) / a0;
    pb[1] = 2.0 * (K * K - Vh) / a0;
    pb[2] = (Vh - Vb * K / Q + K * K) / a0;
    pa[1] = 2.0 * (K * K - 1.0) / a0;
    pa[2] = (1.0 - K / Q + K * K) / a0;
    f0 = 38.13547087602444;
    Q = 0.5003270373238773;
    K = std::tan(M_PI * f0 / m_rate);
    ra[1] = 2.0 * (K * K - 1.0) / (1.0 + K / Q + K * K);
    ra[2] = (1.0 - K / Q + K * K) / (1.0 + K / Q + K * K);
    for (int i = 0; i < 5; ++i) m_b[i] = m_a[i] = 0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            m_b[i + j] += pb[i] * rb[j];
            m_a[i + j] += pa[i] * ra[j];
        }
    m_state.assign(size_t(m_ch) * 4, 0.0);
    m_subLen = m_rate / 10;
    m_subAcc.assign(m_ch, 0.0);
    // 4x oversampling interpolator: 48-tap windowed sinc split in 4 phases of 12 taps.
    const int factor = m_rate < 96000 ? 4 : 2;
    m_taps = 12;
    const int total = m_taps * factor;
    m_phases.assign(factor, std::vector<float>(m_taps));
    for (int k = 0; k < total; ++k) {
        const double x = (k - (total - 1) / 2.0) / factor;
        const double sinc = std::abs(x) < 1e-9 ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
        const double w = 0.5 * (1 - std::cos(2 * M_PI * (k + 0.5) / total));
        m_phases[k % factor][k / factor] = float(sinc * w);
    }
    m_history.assign(size_t(m_ch) * m_taps, 0.f);
}

void LoudnessMeter::add(const float *in, int frames)
{
    for (int f = 0; f < frames; ++f) {
        const float *x = in + size_t(f) * m_ch;
        for (int c = 0; c < m_ch; ++c) {
            const double v = x[c];
            m_samplePeak = std::max(m_samplePeak, std::abs(v));
            // K-weighting (direct form II transposed).
            double *s = &m_state[size_t(c) * 4];
            const double y = m_b[0] * v + s[0];
            s[0] = m_b[1] * v - m_a[1] * y + s[1];
            s[1] = m_b[2] * v - m_a[2] * y + s[2];
            s[2] = m_b[3] * v - m_a[3] * y + s[3];
            s[3] = m_b[4] * v - m_a[4] * y;
            m_subAcc[c] += y * y;
            // True peak.
            float *h = &m_history[size_t(c) * m_taps];
            h[m_histPos] = x[c];
            for (const auto &ph : m_phases) {
                double acc = 0;
                for (int t = 0; t < m_taps; ++t) acc += ph[t] * h[(m_histPos - t + m_taps) % m_taps];
                m_truePeak = std::max(m_truePeak, std::abs(acc));
            }
        }
        m_histPos = (m_histPos + 1) % m_taps;
        if (++m_subPos == m_subLen) finishBlock();
    }
}

void LoudnessMeter::finishBlock()
{
    m_subs.push_back(m_subAcc);
    std::fill(m_subAcc.begin(), m_subAcc.end(), 0.0);
    m_subPos = 0;
    if (m_subs.size() < 4)
        return;
    // Channel weights: 1.0 for front channels, 1.41 for surrounds (5.1: L R C LFE Ls Rs, LFE ignored).
    double sum = 0;
    const size_t n = m_subs.size();
    for (int c = 0; c < m_ch; ++c) {
        double w = 1.0;
        if (m_ch == 6) w = c == 3 ? 0.0 : c >= 4 ? 1.41 : 1.0;
        double e = 0;
        for (size_t k = n - 4; k < n; ++k) e += m_subs[k][c];
        sum += w * e / (4.0 * m_subLen);
    }
    m_blocks.push_back(sum);
    if (m_subs.size() > 8) m_subs.erase(m_subs.begin(), m_subs.end() - 4);
}

double LoudnessMeter::integratedLufs() const
{
    auto lufs = [](double e) { return -0.691 + 10.0 * std::log10(std::max(e, 1e-20)); };
    const double absGate = std::pow(10.0, (-70.0 + 0.691) / 10.0);
    double sum = 0;
    int n = 0;
    for (double b : m_blocks)
        if (b > absGate) { sum += b; ++n; }
    if (!n) return -70.0;
    const double relGate = sum / n * std::pow(10.0, -10.0 / 10.0);
    double sum2 = 0;
    int n2 = 0;
    for (double b : m_blocks)
        if (b > absGate && b > relGate) { sum2 += b; ++n2; }
    return n2 ? lufs(sum2 / n2) : -70.0;
}

double LoudnessMeter::truePeak() const { return std::max(m_truePeak, m_samplePeak); }

} // namespace rc
