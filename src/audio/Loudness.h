#pragma once
// ITU-R BS.1770-4 / EBU R128 loudness meter: integrated loudness (gated,
// K-weighted) and true peak (4x oversampled). Streaming, interleaved float input.
#include <cstdint>
#include <vector>

namespace rc {

class LoudnessMeter {
public:
    LoudnessMeter(int channels, int sampleRate);
    void add(const float *interleaved, int frames);
    double integratedLufs() const;      // -70 or lower when silent
    double truePeak() const;            // linear (1.0 = 0 dBTP)
    double samplePeak() const { return m_samplePeak; }

private:
    void finishBlock();
    int m_ch, m_rate;
    // K-weighting: 4th order IIR (pre-filter * RLB high-pass), per channel state.
    double m_b[5], m_a[5];
    std::vector<double> m_state;        // 4 per channel (direct form II transposed)
    // 100 ms sub-blocks of channel energies; blocks are 4 sub-blocks (400 ms, 75 % overlap).
    int m_subLen;
    int m_subPos = 0;
    std::vector<double> m_subAcc;       // energy per channel for the current sub-block
    std::vector<std::vector<double>> m_subs;
    std::vector<double> m_blocks;       // weighted mean square of each 400 ms block
    // True peak: polyphase interpolation.
    std::vector<std::vector<float>> m_phases;
    std::vector<float> m_history;       // per channel ring of the last taps
    int m_taps = 0, m_histPos = 0;
    double m_truePeak = 0, m_samplePeak = 0;
};

} // namespace rc
