// =============================================================================
//  test_wsola.cpp - time stretch keeps pitch and lands on the expected length
// =============================================================================
#include "test.h"
#include "ttsc.h"

#include <cmath>
#include <vector>

namespace {

ttsc::AudioBuffer tone(double seconds, float freq = 200.0f, int rate = 24000) {
    ttsc::AudioBuffer a;
    a.sample_rate = rate;
    const size_t n = size_t(seconds * rate);
    a.samples.resize(n);
    for (size_t i = 0; i < n; ++i) {
        a.samples[i] = 0.4f * std::sin(2.0f * 3.14159265f * freq * float(i) / float(rate));
    }
    return a;
}

bool all_finite(const std::vector<float>& v) {
    for (float f : v) {
        if (!std::isfinite(f)) return false;
    }
    return true;
}

double rms(const std::vector<float>& v) {
    if (v.empty()) return 0;
    double acc = 0;
    for (float f : v) acc += double(f) * double(f);
    return std::sqrt(acc / double(v.size()));
}

}  // namespace

TTSC_TEST(wsola_rate_one_is_identity) {
    const ttsc::AudioBuffer in  = tone(0.5);
    const ttsc::AudioBuffer out = ttsc::wsola::stretch(in, 1.0f);
    CHECK_EQ(out.samples.size(), in.samples.size());
    for (size_t i = 0; i < in.samples.size(); ++i) {
        if (out.samples[i] != in.samples[i]) {
            CHECK(false);
            break;
        }
    }
}

TTSC_TEST(wsola_faster_shortens) {
    const ttsc::AudioBuffer in  = tone(1.0);
    const ttsc::AudioBuffer out = ttsc::wsola::stretch(in, 1.5f);
    CHECK_EQ(out.sample_rate, 24000);
    const double expect = double(in.samples.size()) / 1.5;
    CHECK_NEAR(double(out.samples.size()), expect, expect * 0.02 + 2.0);
    CHECK(all_finite(out.samples));
    // Stretching must not silence the signal.
    CHECK(rms(out.samples) > 0.1);
}

TTSC_TEST(wsola_slower_lengthens) {
    const ttsc::AudioBuffer in  = tone(0.5);
    const ttsc::AudioBuffer out = ttsc::wsola::stretch(in, 0.5f);
    const double expect = double(in.samples.size()) / 0.5;
    CHECK_NEAR(double(out.samples.size()), expect, expect * 0.02 + 2.0);
    CHECK(all_finite(out.samples));
}

TTSC_TEST(wsola_clamps_extreme_rates) {
    const ttsc::AudioBuffer in = tone(0.2);
    const auto fast = ttsc::wsola::stretch(in, 8.0f);   // clamped to 2.0
    const auto slow = ttsc::wsola::stretch(in, 0.01f);  // clamped to 0.5
    CHECK_NEAR(double(fast.samples.size()), double(in.samples.size()) / 2.0,
               double(in.samples.size()) * 0.05);
    CHECK_NEAR(double(slow.samples.size()), double(in.samples.size()) / 0.5,
               double(in.samples.size()) * 0.05);
}

// A pure sine hides level errors (every window lines up), so this uses a
// speech-ish mix with a decaying envelope. Regression for the shipped bug where
// the first block used gain 1.0 and the next block's ramp added on top: the
// sentence attack reached 2.0 (+6 dB) and clipped into a "square wave" buzz.
TTSC_TEST(wsola_gain_does_not_overshoot) {
    ttsc::AudioBuffer in;
    in.sample_rate = 24000;
    const size_t n = 24000;
    in.samples.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const float t = float(i) / 24000.0f;
        const float env = std::exp(-3.0f * t);
        in.samples[i] = env * (0.30f * std::sin(2.0f * 3.14159265f * 180.0f * t) +
                               0.18f * std::sin(2.0f * 3.14159265f * 420.0f * t) +
                               0.06f * std::sin(2.0f * 3.14159265f * 1750.0f * t));
    }
    for (float rate : {1.15f, 1.25f, 0.9f}) {
        const auto out = ttsc::wsola::stretch(in, rate);
        float peak = 0;
        for (float f : out.samples) peak = std::max(peak, std::fabs(f));
        CHECK(peak < 0.56f);            // input peak is ~0.54; +6 dB would be ~1.08
        CHECK(rms(out.samples) > 0.10); // and not silent
    }
}

// No clicks: consecutive output samples must not jump much more than the input
// already does. A splice that lands out of phase shows up here.
TTSC_TEST(wsola_has_no_splices_that_click) {
    const ttsc::AudioBuffer in = tone(1.0, 220.0f);
    const auto out = ttsc::wsola::stretch(in, 1.3f);
    // Skip the edges: the first/last window are partially normalised, which is
    // a fade, not a splice.
    const size_t guard = 1200;
    float in_step = 0, out_step = 0;
    for (size_t i = 1; i < in.samples.size(); ++i)
        in_step = std::max(in_step, std::fabs(in.samples[i] - in.samples[i - 1]));
    for (size_t i = guard; i + 1 < out.samples.size() - guard; ++i)
        out_step = std::max(out_step, std::fabs(out.samples[i] - out.samples[i - 1]));
    CHECK(out_step < in_step * 2.0f);
}

TTSC_TEST(wsola_empty_input_is_safe) {    ttsc::AudioBuffer empty;
    empty.sample_rate = 24000;
    const auto out = ttsc::wsola::stretch(empty, 1.5f);
    CHECK_EQ(out.samples.size(), size_t(0));
    const auto tiny = ttsc::wsola::stretch([] {
        ttsc::AudioBuffer a;
        a.sample_rate = 24000;
        a.samples.assign(10, 0.0f);
        return a;
    }(), 1.5f);
    CHECK(tiny.samples.size() < size_t(20));   // shorter than a window: no output
}
