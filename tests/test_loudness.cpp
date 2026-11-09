#include <catch2/catch.hpp>
#include "audio/Loudness.h"
#include <cmath>
#include <random>
#include <vector>
#ifdef RACCORD_TEST_EBUR128
#include <ebur128.h>
#endif

using namespace rc;

// 20 s stereo programme: 1 kHz tone, noise bursts, a quiet passage and silence.
static std::vector<float> programme(int rate)
{
    std::vector<float> s(size_t(rate) * 20 * 2);
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0.f, 0.1f);
    for (int i = 0; i < rate * 20; ++i) {
        const double t = double(i) / rate;
        float l = 0, r = 0;
        if (t < 6) { l = float(0.5 * std::sin(2 * M_PI * 1000 * t)); r = l * 0.7f; }
        else if (t < 12) { l = nd(rng); r = nd(rng) * 0.5f; }
        else if (t < 16) { l = float(0.02 * std::sin(2 * M_PI * 440 * t)); r = l; }
        s[size_t(i) * 2] = l;
        s[size_t(i) * 2 + 1] = r;
    }
    return s;
}

TEST_CASE("loudness meter: reference tone")
{
    // 1 kHz sine with an RMS of -20 dBFS on one channel: K-weighting adds +0.69 dB at
    // 1 kHz and the -0.691 offset cancels it, so the programme measures -20.0 LUFS.
    const int rate = 48000;
    std::vector<float> s(size_t(rate) * 10 * 2, 0.f);
    const double amp = std::pow(10.0, -20.0 / 20.0) * std::sqrt(2.0);
    for (int i = 0; i < rate * 10; ++i) s[size_t(i) * 2] = float(amp * std::sin(2 * M_PI * 1000.0 * i / rate));
    LoudnessMeter m(2, rate);
    m.add(s.data(), rate * 10);
    CHECK(std::abs(m.integratedLufs() - (-20.0)) < 0.05);
    CHECK(std::abs(20 * std::log10(m.truePeak()) - 20 * std::log10(amp)) < 0.1);
}

#ifdef RACCORD_TEST_EBUR128
TEST_CASE("loudness meter matches libebur128")
{
    for (int rate : {44100, 48000}) {
        auto s = programme(rate);
        LoudnessMeter m(2, rate);
        m.add(s.data(), rate * 20);
        ebur128_state *st = ebur128_init(2, rate, EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK);
        ebur128_add_frames_float(st, s.data(), size_t(rate) * 20);
        double ref = 0, p0 = 0, p1 = 0;
        ebur128_loudness_global(st, &ref);
        ebur128_true_peak(st, 0, &p0);
        ebur128_true_peak(st, 1, &p1);
        ebur128_destroy(&st);
        INFO("rate " << rate << " ours " << m.integratedLufs() << " ref " << ref);
        CHECK(std::abs(m.integratedLufs() - ref) < 0.05);
        CHECK(std::abs(20 * std::log10(m.truePeak()) - 20 * std::log10(std::max(p0, p1))) < 0.3);
    }
}
#endif
