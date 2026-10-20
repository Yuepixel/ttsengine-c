// =============================================================================
//  wsola.cpp - Waveform Similarity Overlap-Add time stretching
//
//  Why WSOLA and not "just resample":
//    The game asks for a speaking rate (rate > 1 = faster). Plain resampling
//    would shift the pitch and make every character sound like a chipmunk.
//    WSOLA moves whole waveform periods instead, so pitch is preserved.
//
//  How it works (2026-10-20 rewrite)
//    * The output is covered by Hann-windowed blocks that advance by win/2.
//    * Block k's nominal input position is k * (win/2) * rate. Around it we
//      search +-search samples for the segment whose *first half* best matches
//      the "natural continuation" of the previous block (the input right after
//      the previous block's accepted segment). That keeps the phase continuous.
//    * Every block is summed into a gain accumulator; at the end the output is
//      divided by it. Without that normalisation the first block (gain 1.0 in
//      the old code) and the next block's rising ramp summed to 2.0, i.e. +6 dB
//      of clipping on every sentence attack - heard as "音头被吞 / 像方波".
//
//  Tuning notes (2026-10-20, judged by ear):
//    Slowing down (rate < 1) is the weak spot. We tested the hypothesis that a
//    "better" WSOLA (window sweep 30/48/64/85/107 ms, search +-win/2, step 1)
//    would fix it, plus audiotsm, Rubberband (R3) and a phase vocoder, all on
//    the same source waveform. Verdict from the user's ears: only rate 1.0 is
//    normal; every stretched version is worse, the wider windows worse than the
//    original 30 ms setting. So slow-down is a capability boundary of this
//    pipeline, not a parameter bug - the shipped defaults stay 30 ms / +-10 ms /
//    step 2. The Params struct and the --win-ms/--search-ms/--search-step
//    diagnostics are kept so a future attempt does not start from scratch.
//
//  Cost: one search per output window, O(search/step * half) float multiply-adds.
// =============================================================================
#include "ttsc.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ttsc {
namespace wsola {
namespace {

constexpr int kWindowMs      = 30;   // matching window (both directions)
constexpr int kSearchMs      = 10;   // +- search range (both directions)
constexpr int kSearchStep    = 2;    // search step in samples

inline float hann(int i, int n) {
    if (n <= 1) return 1.0f;
    return 0.5f * (1.0f - std::cos(2.0f * 3.14159265358979f * float(i) / float(n - 1)));
}

}  // namespace

AudioBuffer stretch(const AudioBuffer& in, float rate) {
    return stretch(in, rate, Params{});
}

AudioBuffer stretch(const AudioBuffer& in, float rate, const Params& params) {
    // Clamp to the range the HTTP layer accepts; guard against division by zero.
    rate = std::max(0.5f, std::min(2.0f, rate));
    if (std::fabs(rate - 1.0f) < 0.001f || in.samples.empty()) return in;

    const int sr = in.sample_rate > 0 ? in.sample_rate : 24000;

    const int window_ms = params.window_ms > 0 ? params.window_ms : kWindowMs;
    int win = std::max(64, (window_ms * sr) / 1000);
    win &= ~1;  // even, so half is exact
    const int half = win / 2;

    const int search_ms = params.search_ms > 0 ? params.search_ms : kSearchMs;
    const int search = std::max(8, (search_ms * sr) / 1000);

    const int step = params.step > 0 ? params.step : kSearchStep;

    const size_t in_len  = in.samples.size();
    const size_t out_len = size_t(double(in_len) / double(rate));

    AudioBuffer out;
    out.sample_rate = sr;
    out.samples.assign(out_len, 0.0f);
    if (out_len < size_t(win)) {
        // Too short to stretch; at least do not return garbage.
        return in;
    }
    std::vector<float> gain(out_len, 0.0f);

    std::vector<float> win_curve(win);
    for (int i = 0; i < win; ++i) win_curve[i] = hann(i, win);

    const float* src = in.samples.data();
    int     prev_best = -1;    // input index of the last accepted block
    size_t  out_pos   = 0;
    double  in_pos    = 0.0;   // nominal input position of the next block

    while (out_pos + size_t(win) <= out_len) {
        int nominal = int(in_pos + 0.5);
        int best    = nominal;

        if (prev_best >= 0) {
            // Template = what would naturally follow the previous block: the
            // half window right after prev_best + half. Matching against it
            // (instead of against the already-emitted output) keeps the
            // waveform's own phase, so the splice does not click.
            const int tpl = prev_best + half;
            if (tpl + half <= int(in_len)) {
                float       best_score = -2.0f;
                for (int shift = -search; shift <= search; shift += step) {
                    const int cand = nominal + shift;
                    if (cand < 0 || cand + win > int(in_len)) continue;
                    float num = 0, den = 0;
                    for (int i = 0; i < half; ++i) {
                        const float a = src[tpl + i];
                        const float b = src[cand + i];
                        num += a * b;
                        den += a * a + b * b;
                    }
                    const float score = den > 1e-9f ? (2.0f * num / den) : 0.0f;
                    if (score > best_score) { best_score = score; best = cand; }
                    if (best_score > 0.999f) break;
                }
            }
        }
        if (best + win > int(in_len)) best = int(in_len) - win;
        if (best < 0) break;

        for (int i = 0; i < win; ++i) {
            const float g = win_curve[i];
            out.samples[out_pos + size_t(i)] += src[best + i] * g;
            gain[out_pos + size_t(i)] += g;
        }

        prev_best = best;
        out_pos  += size_t(half);
        in_pos   += double(half) * double(rate);
    }

    // Divide by the accumulated window gain so the level is flat from the very
    // first sample (the old code left the first block unnormalised: +6 dB).
    for (size_t i = 0; i < out_len; ++i) {
        if (gain[i] > 1e-3f) out.samples[i] /= gain[i];
    }
    return out;
}

}  // namespace wsola
}  // namespace ttsc
