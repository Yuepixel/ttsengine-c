/* =============================================================================
 *  gen_icon.c - generate src/resources/app.ico (multi-size, 32-bit)
 *
 *  Why a generator instead of a checked-in binary produced by an editor:
 *    - the icon is part of the source tree and must be reproducible;
 *    - no third-party dependency (no ImageMagick / Python / .NET), plain C +
 *      libc, same policy as the rest of the project;
 *    - the artwork is a rounded square with a speech-waveform glyph, so it can
 *      be described analytically and rendered with 4x4 supersampling.
 *
 *  Output: one .ico holding 256/128/64/48/32/16 frames, each a 32-bit
 *  BITMAPINFOHEADER DIB (Vista+ also accepts PNG frames; DIB is used here so
 *  the file stays dependency-free).
 *
 *  Build & run:
 *    gcc -O2 -o gen_icon.exe tools/gen_icon.c
 *    tools/gen_icon.exe src/resources/app.ico
 * ========================================================================== */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SS 4 /* supersampling factor per axis */

typedef struct { float r, g, b, a; } rgba;

/* Rounded-rectangle signed distance (negative inside). */
static float sd_round_rect(float px, float py, float hx, float hy, float r) {
    float qx = fabsf(px) - hx + r;
    float qy = fabsf(py) - hy + r;
    float mx = qx > 0.0f ? qx : 0.0f;
    float my = qy > 0.0f ? qy : 0.0f;
    float m = qx > qy ? qx : qy;
    if (m > 0.0f) m = 0.0f;
    return m + sqrtf(mx * mx + my * my) - r;
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Waveform geometry, normalised to the icon square (0..1, y downwards). */
typedef struct { float cx, hw, hh; } bar;

static const bar k_bars[5] = {
    {0.3025f, 0.0425f, 0.115f},
    {0.4225f, 0.0425f, 0.195f},
    {0.5425f, 0.0425f, 0.285f},
    {0.6625f, 0.0425f, 0.195f},
    {0.7825f, 0.0425f, 0.115f},
};
#define N_BARS ((int)(sizeof(k_bars) / sizeof(k_bars[0])))

static const float k_radius = 0.215f; /* corner radius of the plate */

/* One sample of the artwork. Returns a straight-alpha colour. */
static rgba sample(float u, float v) {
    rgba out;
    out.r = out.g = out.b = 0.0f;
    out.a = 0.0f;

    /* plate */
    if (sd_round_rect(u - 0.5f, v - 0.5f, 0.5f, 0.5f, k_radius) > 0.0f) return out;

    /* vertical gradient: teal (top) -> blue -> indigo (bottom) */
    float t = clampf(v, 0.0f, 1.0f);
    float c0[3] = {0.196f, 0.831f, 0.760f}; /* #32D4C2 */
    float c1[3] = {0.235f, 0.478f, 0.965f}; /* #3C7AF6 */
    float c2[3] = {0.290f, 0.231f, 0.792f}; /* #4A3BCA */
    float a = t < 0.55f ? t / 0.55f : (t - 0.55f) / 0.45f;
    const float *lo = t < 0.55f ? c0 : c1;
    const float *hi = t < 0.55f ? c1 : c2;
    out.r = lo[0] + (hi[0] - lo[0]) * a;
    out.g = lo[1] + (hi[1] - lo[1]) * a;
    out.b = lo[2] + (hi[2] - lo[2]) * a;
    out.a = 1.0f;

    /* waveform bars, white, pill shaped */
    for (int i = 0; i < N_BARS; i++) {
        float d = sd_round_rect(u - k_bars[i].cx, v - 0.5f, k_bars[i].hw,
                                k_bars[i].hh, k_bars[i].hw);
        if (d <= 0.0f) {
            out.r = out.g = out.b = 1.0f;
            out.a = 1.0f;
        }
    }
    return out;
}

/* Render one frame; pixels are straight-alpha RGBA, top-down. */
static void render(int size, uint8_t *dst) {
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            float ar = 0.0f, ag = 0.0f, ab = 0.0f, aa = 0.0f;
            for (int sy = 0; sy < SS; sy++) {
                for (int sx = 0; sx < SS; sx++) {
                    float u = ((float)x + ((float)sx + 0.5f) / SS) / (float)size;
                    float v = ((float)y + ((float)sy + 0.5f) / SS) / (float)size;
                    rgba c = sample(u, v);
                    ar += c.r * c.a;
                    ag += c.g * c.a;
                    ab += c.b * c.a;
                    aa += c.a;
                }
            }
            float n = (float)(SS * SS);
            float alpha = aa / n;
            uint8_t *p = dst + ((size_t)y * size + x) * 4;
            if (alpha <= 0.0001f) {
                p[0] = p[1] = p[2] = p[3] = 0;
            } else {
                p[0] = (uint8_t)(clampf(ar / aa, 0.0f, 1.0f) * 255.0f + 0.5f);
                p[1] = (uint8_t)(clampf(ag / aa, 0.0f, 1.0f) * 255.0f + 0.5f);
                p[2] = (uint8_t)(clampf(ab / aa, 0.0f, 1.0f) * 255.0f + 0.5f);
                p[3] = (uint8_t)(alpha * 255.0f + 0.5f);
            }
        }
    }
}

static void put16(uint8_t *p, uint16_t v);
static void put32(uint8_t *p, uint32_t v);
static void put32be(uint8_t *p, uint32_t v); /* PNG chunks are big-endian */

/* ---- minimal PNG writer (stored deflate blocks, no compression library) ----
 * The 256 px frame is stored as PNG: that is what modern icon editors emit and
 * what the shell/GDI+ expect for the largest size. Frames this large are the
 * only ones where a 32-bit DIB wastes space anyway (256*256*4 = 256 KiB).
 */
static uint32_t crc_table[256];
static int crc_ready = 0;

static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
    crc_ready = 1;
}

static uint32_t crc32_buf(const uint8_t *buf, size_t len) {
    uint32_t c = 0xFFFFFFFFu;
    if (!crc_ready) crc_init();
    for (size_t i = 0; i < len; i++) c = crc_table[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static uint32_t adler32_buf(const uint8_t *buf, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + buf[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

typedef struct { uint8_t *p; size_t len, cap; } buf_t;

static void buf_put(buf_t *b, const void *src, size_t n) {
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->len + n) cap *= 2;
        b->p = (uint8_t *)realloc(b->p, cap);
        if (!b->p) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
        b->cap = cap;
    }
    memcpy(b->p + b->len, src, n);
    b->len += n;
}

static void png_chunk(buf_t *out, const char *type, const uint8_t *data, size_t n) {
    uint8_t hdr[8];
    put32be(hdr, (uint32_t)n);
    memcpy(hdr + 4, type, 4);
    buf_put(out, hdr, 8);
    if (n) buf_put(out, data, n);
    /* CRC covers the type and the payload, not the length. */
    buf_t tmp = {0};
    buf_put(&tmp, type, 4);
    if (n) buf_put(&tmp, data, n);
    uint32_t crc = crc32_buf(tmp.p, tmp.len);
    free(tmp.p);
    uint8_t tail[4];
    put32be(tail, crc);
    buf_put(out, tail, 4);
}

/* Render straight-alpha RGBA (top-down) to a PNG blob. */
static void encode_png(const uint8_t *rgba, int size, buf_t *out) {
    const size_t stride = (size_t)size * 4 + 1;
    const size_t raw_len = stride * (size_t)size;
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    uint8_t *z = (uint8_t *)malloc(raw_len + raw_len / 65535 * 5 + 16);
    size_t zlen = 0;
    if (!raw || !z) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    for (int y = 0; y < size; y++) {
        raw[(size_t)y * stride] = 0; /* filter: none */
        memcpy(raw + (size_t)y * stride + 1, rgba + (size_t)y * size * 4,
               (size_t)size * 4);
    }

    z[zlen++] = 0x78; /* zlib: deflate, 32K window */
    z[zlen++] = 0x01;
    size_t pos = 0;
    do {
        size_t n = raw_len - pos;
        if (n > 65535) n = 65535;
        int final = (pos + n == raw_len);
        z[zlen++] = (uint8_t)(final ? 1 : 0); /* stored block, no compression */
        z[zlen++] = (uint8_t)(n & 0xFF);
        z[zlen++] = (uint8_t)(n >> 8);
        z[zlen++] = (uint8_t)(~n & 0xFF);
        z[zlen++] = (uint8_t)((~n >> 8) & 0xFF);
        memcpy(z + zlen, raw + pos, n);
        zlen += n;
        pos += n;
    } while (pos < raw_len);
    uint32_t adler = adler32_buf(raw, raw_len);
    z[zlen++] = (uint8_t)(adler >> 24);
    z[zlen++] = (uint8_t)(adler >> 16);
    z[zlen++] = (uint8_t)(adler >> 8);
    z[zlen++] = (uint8_t)(adler);

    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    buf_put(out, sig, 8);
    uint8_t ihdr[13];
    put32be(ihdr, (uint32_t)size);
    put32be(ihdr + 4, (uint32_t)size);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 6;  /* colour type: RGBA */
    ihdr[10] = 0; /* deflate */
    ihdr[11] = 0; /* adaptive filtering */
    ihdr[12] = 0; /* no interlace */
    png_chunk(out, "IHDR", ihdr, sizeof(ihdr));
    png_chunk(out, "IDAT", z, zlen);
    png_chunk(out, "IEND", NULL, 0);
    free(raw);
    free(z);
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void put32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xFF);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >> 8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
}

int main(int argc, char **argv) {
    static const int sizes[] = {256, 128, 64, 48, 32, 16};
    const int count = (int)(sizeof(sizes) / sizeof(sizes[0]));
    const char *path = argc > 1 ? argv[1] : "src/resources/app.ico";
    /* Optional second argument "dib": keep every frame a 32-bit DIB (debug aid
     * for icon readers that cannot decode PNG-compressed frames). */
    const int force_dib = (argc > 2 && strcmp(argv[2], "dib") == 0);

    uint8_t *frames[16];
    uint32_t frame_len[16];
    uint32_t offset;
    FILE *f;

    for (int i = 0; i < count; i++) {
        int s = sizes[i];
        uint8_t *px = (uint8_t *)malloc((size_t)s * s * 4);
        if (!px) {
            fprintf(stderr, "out of memory\n");
            return 1;
        }
        render(s, px);

        if (s >= 256 && !force_dib) {
            buf_t png = {0};
            encode_png(px, s, &png);
            frames[i] = png.p;
            frame_len[i] = (uint32_t)png.len;
            free(px);
            continue;
        }

        size_t xor_len = (size_t)s * s * 4;
        size_t and_row = ((size_t)s + 31) / 32 * 4; /* 1bpp rows, 4-byte padded */
        size_t and_len = and_row * (size_t)s;
        frame_len[i] = (uint32_t)(40 + xor_len + and_len);
        frames[i] = (uint8_t *)calloc(1, frame_len[i]);
        if (!frames[i]) {
            fprintf(stderr, "out of memory\n");
            return 1;
        }

        uint8_t *hdr = frames[i];
        put32(hdr + 0, 40);          /* biSize */
        put32(hdr + 4, (uint32_t)s); /* biWidth */
        put32(hdr + 8, (uint32_t)s * 2u); /* biHeight: XOR + AND */
        put16(hdr + 12, 1);          /* biPlanes */
        put16(hdr + 14, 32);         /* biBitCount */
        put32(hdr + 20, (uint32_t)xor_len);

        /* BGRA, bottom-up */
        uint8_t *xor_map = hdr + 40;
        for (int y = 0; y < s; y++) {
            const uint8_t *src = px + (size_t)(s - 1 - y) * s * 4;
            uint8_t *drow = xor_map + (size_t)y * s * 4;
            for (int x = 0; x < s; x++) {
                drow[x * 4 + 0] = src[x * 4 + 2]; /* B */
                drow[x * 4 + 1] = src[x * 4 + 1]; /* G */
                drow[x * 4 + 2] = src[x * 4 + 0]; /* R */
                drow[x * 4 + 3] = src[x * 4 + 3]; /* A */
            }
        }
        /* AND mask stays all zero: alpha carries the shape. */
        free(px);
    }

    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        return 1;
    }
    uint8_t dir[6];
    put16(dir + 0, 0);
    put16(dir + 2, 1);
    put16(dir + 4, (uint16_t)count);
    fwrite(dir, 1, sizeof(dir), f);

    offset = 6 + 16 * (uint32_t)count;
    for (int i = 0; i < count; i++) {
        uint8_t e[16];
        int s = sizes[i];
        e[0] = (uint8_t)(s >= 256 ? 0 : s);
        e[1] = (uint8_t)(s >= 256 ? 0 : s);
        e[2] = 0;
        e[3] = 0;
        put16(e + 4, 1);
        put16(e + 6, 32);
        put32(e + 8, frame_len[i]);
        put32(e + 12, offset);
        fwrite(e, 1, sizeof(e), f);
        offset += frame_len[i];
    }
    for (int i = 0; i < count; i++) {
        fwrite(frames[i], 1, frame_len[i], f);
        free(frames[i]);
    }
    fclose(f);

    printf("wrote %s (%d frames: 256/128/64/48/32/16)\n", path, count);
    return 0;
}
