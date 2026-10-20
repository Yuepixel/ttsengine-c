// =============================================================================
//  test_wav.cpp - encode/decode round trip and the reference resampler
// =============================================================================
#include "test.h"
#include "ttsc.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

ttsc::AudioBuffer tone(double seconds, int rate = 24000, float freq = 220.0f) {
    ttsc::AudioBuffer a;
    a.sample_rate = rate;
    const size_t n = size_t(seconds * rate);
    a.samples.resize(n);
    for (size_t i = 0; i < n; ++i) {
        a.samples[i] = 0.5f * std::sin(2.0f * 3.14159265f * freq * float(i) / float(rate));
    }
    return a;
}

}  // namespace

TTSC_TEST(wav_roundtrip_16bit) {
    const ttsc::AudioBuffer in = tone(0.25);
    const std::vector<uint8_t> bytes = ttsc::wav_encode(in);
    CHECK_EQ(bytes.size(), size_t(44 + in.samples.size() * 2));
    CHECK(bytes.size() > 44);

    const ttsc::AudioBuffer out = ttsc::wav_decode(bytes.data(), bytes.size());
    CHECK_EQ(out.sample_rate, 24000);
    CHECK_EQ(out.samples.size(), in.samples.size());
    double err = 0;
    for (size_t i = 0; i < out.samples.size(); ++i) {
        err = std::max(err, double(std::fabs(out.samples[i] - in.samples[i])));
    }
    CHECK(err < 1.0 / 32767.0 * 2.0);   // quantisation noise only
}

TTSC_TEST(wav_encode_clips_out_of_range) {
    ttsc::AudioBuffer in;
    in.sample_rate = 24000;
    in.samples = {-2.0f, 2.0f, 0.0f};
    const auto bytes = ttsc::wav_encode(in);
    const auto out   = ttsc::wav_decode(bytes.data(), bytes.size());
    CHECK_EQ(out.samples.size(), size_t(3));
    CHECK(out.samples[0] >= -1.0f && out.samples[0] <= -0.999f);
    CHECK(out.samples[1] <= 1.0f && out.samples[1] >= 0.999f);
}

TTSC_TEST(wav_decode_rejects_non_wav) {
    const uint8_t junk[64] = {0};
    CHECK_THROWS(ttsc::wav_decode(junk, sizeof(junk)));
    CHECK_THROWS(ttsc::wav_decode(nullptr, 0));
}

TTSC_TEST(wav_encode_empty_and_default_rate) {
    ttsc::AudioBuffer empty;
    empty.sample_rate = 0;
    const auto bytes = ttsc::wav_encode(empty);
    CHECK_EQ(bytes.size(), size_t(44));
    const auto out = ttsc::wav_decode(bytes.data(), bytes.size());
    CHECK_EQ(out.sample_rate, 24000);   // default when unset
    CHECK_EQ(out.samples.size(), size_t(0));
}

TTSC_TEST(resample_changes_rate_and_length) {
    const ttsc::AudioBuffer in = tone(0.5, 24000);
    const ttsc::AudioBuffer out = ttsc::resample_linear(in, 16000);
    CHECK_EQ(out.sample_rate, 16000);
    CHECK_EQ(out.samples.size(), size_t(8000));   // 0.5 s at 16 kHz
    // No-op path returns the buffer untouched.
    const ttsc::AudioBuffer same = ttsc::resample_linear(in, 24000);
    CHECK_EQ(same.samples.size(), in.samples.size());
}
