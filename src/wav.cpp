// =============================================================================
//  wav.cpp - RIFF/WAVE decode + encode, and a linear resampler
// =============================================================================
#include "ttsc.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace ttsc {
namespace {

uint32_t rd_u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint16_t rd_u16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

void wr_u32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t((x >> (8 * i)) & 0xFF));
}
void wr_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x & 0xFF));
    v.push_back(uint8_t((x >> 8) & 0xFF));
}

constexpr uint16_t kFormatPcm = 1;
constexpr uint16_t kFormatFloat = 3;
constexpr uint16_t kFormatExtensible = 0xFFFE;

}  // namespace

AudioBuffer wav_decode(const uint8_t* data, size_t size) {
    if (!data || size < 44) throw std::runtime_error("WAV: file too small");
    if (std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0)
        throw std::runtime_error("WAV: not a RIFF/WAVE file");

    AudioBuffer out;
    uint16_t    format = 0, channels = 0, bits = 0;
    uint32_t    rate = 0;
    const uint8_t* pcm = nullptr;
    size_t      pcm_bytes = 0;

    size_t pos = 12;
    while (pos + 8 <= size) {
        const uint8_t* chunk = data + pos;
        const uint32_t len   = rd_u32(chunk + 4);
        const size_t   body  = pos + 8;
        if (body + len > size) break;  // truncated file: stop at the last intact chunk

        if (std::memcmp(chunk, "fmt ", 4) == 0 && len >= 16) {
            format   = rd_u16(data + body);
            channels = rd_u16(data + body + 2);
            rate     = rd_u32(data + body + 4);
            bits     = rd_u16(data + body + 14);
            // WAVE_FORMAT_EXTENSIBLE stores the real format in the GUID tail.
            if (format == kFormatExtensible && len >= 40) format = rd_u16(data + body + 24);
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            pcm       = data + body;
            pcm_bytes = len;
        }
        pos = body + len + (len & 1);  // chunks are word aligned
    }

    if (!pcm || channels == 0 || rate == 0 || bits == 0)
        throw std::runtime_error("WAV: missing fmt/data chunk");

    const size_t frames = pcm_bytes / (size_t(channels) * (bits / 8));
    out.sample_rate = int(rate);
    out.samples.resize(frames);

    for (size_t i = 0; i < frames; ++i) {
        float acc = 0;
        for (uint16_t c = 0; c < channels; ++c) {
            const size_t  off = (i * channels + c) * (bits / 8);
            const uint8_t* s  = pcm + off;
            float v = 0;
            if (format == kFormatPcm) {
                switch (bits) {
                    case 8:  v = (int(s[0]) - 128) / 128.0f; break;              // unsigned
                    case 16: v = int16_t(rd_u16(s)) / 32768.0f; break;
                    case 24: {
                        int32_t x = (int32_t(s[2]) << 16) | (int32_t(s[1]) << 8) | int32_t(s[0]);
                        if (x & 0x800000) x |= ~0xFFFFFF;                         // sign extend
                        v = float(x) / 8388608.0f;
                        break;
                    }
                    case 32: v = float(int32_t(rd_u32(s))) / 2147483648.0f; break;
                    default: throw std::runtime_error("WAV: unsupported PCM bit depth");
                }
            } else if (format == kFormatFloat && (bits == 32 || bits == 64)) {
                if (bits == 32) { float f; std::memcpy(&f, s, 4); v = f; }
                else            { double d; std::memcpy(&d, s, 8); v = float(d); }
            } else {
                throw std::runtime_error("WAV: unsupported encoding (PCM or float only)");
            }
            acc += v;
        }
        out.samples[i] = acc / float(channels);  // downmix to mono
    }
    return out;
}

std::vector<uint8_t> wav_encode(const AudioBuffer& audio) {
    const uint32_t rate     = uint32_t(audio.sample_rate > 0 ? audio.sample_rate : 24000);
    const uint32_t n        = uint32_t(audio.samples.size());
    const uint32_t data_len = n * 2;

    std::vector<uint8_t> out;
    out.reserve(44 + data_len);
    out.insert(out.end(), {'R', 'I', 'F', 'F'});
    wr_u32(out, 36 + data_len);
    out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    wr_u32(out, 16);
    wr_u16(out, kFormatPcm);
    wr_u16(out, 1);              // mono
    wr_u32(out, rate);
    wr_u32(out, rate * 2);       // byte rate
    wr_u16(out, 2);              // block align
    wr_u16(out, 16);             // bits
    out.insert(out.end(), {'d', 'a', 't', 'a'});
    wr_u32(out, data_len);

    for (float f : audio.samples) {
        f = std::max(-1.0f, std::min(1.0f, f));      // hard clip: int16 has no headroom
        const int32_t v = int32_t(std::lround(f * 32767.0f));
        wr_u16(out, uint16_t(int16_t(v)));
    }
    return out;
}

AudioBuffer resample_linear(const AudioBuffer& in, int target_rate) {
    if (in.sample_rate == target_rate || in.samples.empty()) return in;

    const double ratio = double(target_rate) / double(in.sample_rate);
    const size_t n_out = size_t(std::llround(double(in.samples.size()) * ratio));
    AudioBuffer  out;
    out.sample_rate = target_rate;
    out.samples.resize(n_out);

    for (size_t i = 0; i < n_out; ++i) {
        const double src = double(i) / ratio;
        const size_t i0  = size_t(src);
        const size_t i1  = std::min(i0 + 1, in.samples.size() - 1);
        const float  t   = float(src - double(i0));
        out.samples[i]   = in.samples[i0] * (1.0f - t) + in.samples[i1] * t;
    }
    return out;
}

}  // namespace ttsc
