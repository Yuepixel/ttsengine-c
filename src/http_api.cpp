// =============================================================================
//  http_api.cpp - HTTP surface over the engine
//
//  This is the drop-in replacement for the legacy Python server's contract:
//    GET    /health  /perf  /voices
//    POST   /voices/new  /voices/clone  /tts  /tts/pcm
//    DELETE /voices/{id}
//
//  Compatibility notes (checked against the legacy Python server):
//    * errors use FastAPI's shape, {"detail": "..."} with the matching status;
//    * /tts returns 24 kHz mono 16-bit WAV;
//    * /tts/pcm returns the frozen QTTS wire format, all little endian:
//        16 B header: "QTTS" | u8 ver=1 | u8 bits=16 | u8 channels=1 | u8 flags=0
//                     | u32 sample_rate | u32 total=0
//        body       : int16 mono PCM
//        8 B  tail  : "QEND" | u32 total_samples
//      A client only trusts a stream when QEND arrives; otherwise it drops the
//      partial audio and retries through /tts. So a failure after the header
//      must close the connection *without* QEND.
//    * a rate change on /tts/pcm is refused with 422 by policy
//      (Engine::stream_usable, ini key stream_stretch); the game then falls
//      back to buffered /tts, exactly like with the Python server.
// =============================================================================
#include "ttsc.h"

#include "httplib.h"
#include "yyjson.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace ttsc {
namespace {

constexpr const char* kServerVersion  = "0.3.0";
constexpr const char* kPcmProtocol    = "pcm16-v1";
constexpr const char* kPcmMagic       = "QTTS";
constexpr const char* kPcmEnd         = "QEND";
constexpr int         kSampleRate     = 24000;
constexpr size_t      kMaxTextChars   = 500;       // legacy pydantic max_length
constexpr size_t      kMaxInstruct    = 200;
constexpr size_t      kMaxRefB64Chars = 28000000;  // ~20 MB of audio
constexpr double      kMaxRefSeconds  = 90.0;
constexpr size_t      kPayloadMax     = 32u * 1024u * 1024u;

// -----------------------------------------------------------------------------
//  small helpers
// -----------------------------------------------------------------------------
using Clock = std::chrono::steady_clock;

double ms_since(const Clock::time_point& t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    o += buf;
                } else {
                    o.push_back(char(c));
                }
        }
    }
    return o;
}

// Counts Unicode code points, which is what the Python server's min_length /
// max_length counted. Counting bytes would cap Chinese text at 167 characters.
size_t utf8_length(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++n;
    }
    return n;
}

void send_json(httplib::Response& res, int status, const std::string& body) {
    res.status = status;
    res.set_content(body, "application/json");
}

// FastAPI compatible error envelope. The game only looks at the status code,
// but the GUI and the probe scripts display `detail`.
void send_error(httplib::Response& res, int status, const std::string& msg) {
    send_json(res, status, "{\"detail\":\"" + json_escape(msg) + "\"}");
}

std::string voice_json(const Voice& v, const std::string& ref_audio) {
    std::string o = "{";
    o += "\"id\":\"" + json_escape(v.id) + "\",";
    o += "\"type\":\"" + json_escape(v.type) + "\",";
    o += "\"description\":\"" + json_escape(v.description) + "\",";
    o += "\"script\":\"" + json_escape(v.script) + "\",";
    o += "\"language\":\"" + json_escape(v.language) + "\",";
    o += "\"ref_audio\":\"" + json_escape(ref_audio) + "\",";
    o += "\"ref_text\":\"" + json_escape(v.ref_text) + "\",";
    o += "\"x_vector_only\":" + std::string(v.x_vector_only ? "true" : "false") + ",";
    o += "\"created\":\"" + json_escape(v.created) + "\"";
    o += "}";
    return o;
}

// Standard base64 with optional padding.
bool base64_decode(const std::string& in, std::string* out) {
    static int8_t table[256];
    static std::once_flag once;
    std::call_once(once, [] {
        std::memset(table, -1, sizeof(table));
        const char* alpha =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) table[static_cast<unsigned char>(alpha[i])] = int8_t(i);
    });

    out->clear();
    out->reserve(in.size() / 4 * 3);
    uint32_t acc  = 0;
    int      bits = 0;
    for (char ch : in) {
        if (ch == '=' || ch == '\n' || ch == '\r' || ch == ' ' || ch == '\t') continue;
        const int8_t v = table[static_cast<unsigned char>(ch)];
        if (v < 0) return false;
        acc = (acc << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(char((acc >> bits) & 0xFF));
        }
    }
    return true;
}

// -----------------------------------------------------------------------------
//  concurrency slots - the legacy max_queue guard
// -----------------------------------------------------------------------------
//  The ABI already queues work internally (max_batch sequences in flight), so
//  this is not about engine safety: it stops a pawn burst from stacking twenty
//  seconds of GPU work behind one request. Exceeding it answers a clean 429
//  *before* any response header is sent, so the game can retry later.
class Slots {
public:
    explicit Slots(int limit) : limit_(limit > 0 ? limit : 1) {}

    bool try_acquire() {
        int n = n_.load(std::memory_order_relaxed);
        while (n < limit_) {
            if (n_.compare_exchange_weak(n, n + 1, std::memory_order_acq_rel)) return true;
        }
        return false;
    }
    void release() { n_.fetch_sub(1, std::memory_order_acq_rel); }
    int  current() const { return n_.load(std::memory_order_relaxed); }
    int  limit() const { return limit_; }

private:
    std::atomic<int> n_{0};
    const int        limit_;
};

struct SlotGuard {
    Slots* slots;
    ~SlotGuard() { slots->release(); }
};

std::string busy_message(const Slots& slots) {
    return "server busy (" + std::to_string(slots.current()) + "/" +
           std::to_string(slots.limit()) + "), retry later";
}

// -----------------------------------------------------------------------------
//  request parsing
// -----------------------------------------------------------------------------
struct TtsBody {
    std::string text;
    std::string voice;
    std::string language;
    float       rate       = 1.0f;
    std::string instruct;
    float       temperature = 0;
    int         top_k       = 0;
    float       top_p       = 0;
    float       repetition  = 0;
};

// Reads an optional JSON string field. False means "present but wrong type".
bool get_str(yyjson_val* obj, const char* key, std::string* out) {
    yyjson_val* v = yyjson_obj_get(obj, key);
    if (!v || yyjson_is_null(v)) return true;
    if (!yyjson_is_str(v)) return false;
    *out = yyjson_get_str(v);
    return true;
}

bool get_num(yyjson_val* obj, const char* key, double* out) {
    yyjson_val* v = yyjson_obj_get(obj, key);
    if (!v || yyjson_is_null(v)) return true;
    if (!yyjson_is_num(v)) return false;
    *out = yyjson_get_num(v);
    return true;
}

// Parses and validates a TtsRequest. On failure `err` carries a client-safe
// message and `status` the HTTP code to answer with.
bool parse_tts_body(const std::string& body, TtsBody* out, std::string* err, int* status) {
    yyjson_doc* doc = yyjson_read(body.c_str(), body.size(), 0);
    if (!doc) {
        *err = "invalid JSON body";
        *status = 400;
        return false;
    }
    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        *err = "request body must be a JSON object";
        *status = 400;
        return false;
    }

    bool ok = true;
    ok = ok && get_str(root, "text", &out->text);
    ok = ok && get_str(root, "voice", &out->voice);
    ok = ok && get_str(root, "language", &out->language);
    ok = ok && get_str(root, "instruct", &out->instruct);

    double d = 0;
    if (ok) {
        d = out->rate;
        ok = get_num(root, "rate", &d);
        out->rate = float(d);
    }
    if (ok) {
        d = 0;
        ok = get_num(root, "temperature", &d);
        out->temperature = float(d);
    }
    if (ok) {
        d = 0;
        ok = get_num(root, "top_k", &d);
        out->top_k = int(d);
    }
    if (ok) {
        d = 0;
        ok = get_num(root, "top_p", &d);
        out->top_p = float(d);
    }
    if (ok) {
        d = 0;
        ok = get_num(root, "repetition_penalty", &d);
        out->repetition = float(d);
    }
    yyjson_doc_free(doc);

    if (!ok) {
        *err = "field type mismatch in request body";
        *status = 400;
        return false;
    }
    if (out->text.empty() || trim(out->text).empty()) {
        *err = "text cannot be empty";
        *status = 400;
        return false;
    }
    if (utf8_length(out->text) > kMaxTextChars) {
        *err = "text is longer than 500 characters";
        *status = 400;
        return false;
    }
    if (utf8_length(out->instruct) > kMaxInstruct) {
        *err = "instruct is longer than 200 characters";
        *status = 400;
        return false;
    }
    // The legacy model declared rate ge=0.5 le=2.0, so FastAPI answered 422.
    if (!(out->rate >= 0.5f && out->rate <= 2.0f)) {
        *err = "rate must be within [0.5, 2.0]";
        *status = 422;
        return false;
    }
    return true;
}

// Debug breadcrumb: written straight to fd 1. In the GUI build stdout is a
// redirected pipe whose FILE* can be stale (the window is a GUI subsystem
// process), so _write is the only reliable way to get the line into the pane.
void dbg_line(const std::string& s) {
    const std::string line = s + "\n";
    _write(1, line.data(), unsigned(line.size()));
}

// Resolves the voice id, defaulting to the first registered voice exactly like
// the Python server did.
bool resolve_voice(const ServerContext& ctx, const std::string& id, Voice* out,
                   std::string* err) {
    if (!id.empty()) {
        if (ctx.voices->get(id, out)) return true;
        *err = "voice '" + id + "' does not exist, create it with POST /voices/new";
        return false;
    }
    const auto all = ctx.voices->list();
    if (all.empty()) {
        *err = "no voices registered";
        return false;
    }
    *out = all.front();
    return true;
}

Engine::Request make_request(const TtsBody& b, const Voice& v, const Config& cfg) {
    Engine::Request req;
    req.text     = b.text;
    req.language = b.language.empty()
                       ? (v.language.empty() ? cfg.language : v.language)
                       : b.language;
    req.instruct = b.instruct;
    req.rate     = b.rate;
    req.temperature        = b.temperature;
    req.top_k              = b.top_k;
    req.top_p              = b.top_p;
    req.repetition_penalty = b.repetition;
    return req;
}

void record_perf(ServerContext& ctx, Perf::Sample* s) {
    if (ctx.perf && ctx.cfg.perf) ctx.perf->record(*s);
}

// -----------------------------------------------------------------------------
//  /tts - buffered synthesis
// -----------------------------------------------------------------------------
void handle_tts(ServerContext& ctx, Slots& slots, const httplib::Request& http_req,
                httplib::Response& res) {
    const Clock::time_point t0 = Clock::now();

    TtsBody body;
    std::string err;
    int status = 400;
    if (!parse_tts_body(http_req.body, &body, &err, &status)) {
        send_error(res, status, err);
        return;
    }

    Voice voice;
    if (!resolve_voice(ctx, body.voice, &voice, &err)) {
        send_error(res, 404, err);
        return;
    }

    if (!slots.try_acquire()) {
        send_error(res, 429, busy_message(slots));
        return;
    }
    SlotGuard guard{&slots};

    if (ctx.cfg.debug) {
        // Debug breadcrumb: what the client actually asked for. stdout is
        // captured by the GUI log pane, so a game session can be diagnosed
        // without a debugger (voice / rate / length / emotion preset).
        std::string line = "[tts] " + (body.voice.empty() ? std::string("(default)") : body.voice)
                         + " rate=" + std::to_string(body.rate)
                         + " chars=" + std::to_string(utf8_length(body.text))
                         + " lang=" + body.language
                         + (body.instruct.empty() ? std::string()
                                                  : " instruct=" + body.instruct.substr(0, 32));
        dbg_line(line);
    }

    Perf::Sample sample;
    sample.kind     = "tts";
    sample.text_len = utf8_length(body.text);
    sample.rate     = body.rate;

    AudioBuffer audio;
    try {
        ctx.engine->synthesize(voice, make_request(body, voice, ctx.cfg), &audio);
    } catch (const TimeoutError& e) {
        sample.total_ms = ms_since(t0);
        sample.ok       = false;
        record_perf(ctx, &sample);
        send_error(res, 504, e.what());
        return;
    } catch (const std::exception& e) {
        sample.total_ms = ms_since(t0);
        sample.ok       = false;
        record_perf(ctx, &sample);
        send_error(res, 500, std::string("synthesis failed: ") + e.what());
        return;
    }

    const auto wav = wav_encode(audio);
    sample.total_ms  = ms_since(t0);
    sample.audio_sec = audio.sample_rate > 0
                           ? double(audio.samples.size()) / double(audio.sample_rate)
                           : 0;
    record_perf(ctx, &sample);

    if (ctx.cfg.debug) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "[tts] 200 %zu bytes, %zu samples @%d Hz, %d ms",
                      wav.size(), audio.samples.size(), audio.sample_rate,
                      int(sample.total_ms));
        dbg_line(buf);
    }

    std::string bytes(reinterpret_cast<const char*>(wav.data()), wav.size());
    res.set_content(std::move(bytes), "audio/wav");
}

// -----------------------------------------------------------------------------
//  /tts/pcm - QTTS framed int16 stream
// -----------------------------------------------------------------------------
//  Synthesis runs on its own thread while the connection thread drains a byte
//  queue. Decoupling matters twice: a slow client must not stall the ABI
//  worker (head-of-line blocking across the whole batch), and a client
//  disconnect must abort decoding instead of finishing a stream nobody reads.
struct PcmStream {
    std::mutex              mu;
    std::condition_variable cv;
    std::string             pending;
    size_t                  samples = 0;
    bool                    done    = false;
    bool                    failed  = false;
    bool                    timed_out = false;
    std::string             err;
    std::atomic<bool>       client_gone{false};
    bool                    header_sent = false;   // provider thread only
    std::thread             th;
};

void handle_tts_pcm(ServerContext& ctx, Slots& slots, const httplib::Request& http_req,
                    httplib::Response& res) {
    const Clock::time_point t0 = Clock::now();

    TtsBody body;
    std::string err;
    int status = 400;
    if (!parse_tts_body(http_req.body, &body, &err, &status)) {
        send_error(res, status, err);
        return;
    }
    if (!ctx.cfg.stream) {
        send_error(res, 404, "streaming disabled (ttsserver.ini: stream = 0)");
        return;
    }

    Voice voice;
    if (!resolve_voice(ctx, body.voice, &voice, &err)) {
        send_error(res, 404, err);
        return;
    }

    const Engine::Request req = make_request(body, voice, ctx.cfg);
    if (!ctx.engine->stream_usable(req)) {
        // WSOLA needs the whole waveform; stretching chunk by chunk clicks at
        // the block boundaries. Same 422 as the Python server, and the game
        // falls back to /tts on it.
        send_error(res, 422, "this rate needs WSOLA; use POST /tts instead");
        return;
    }

    // Take the slot before the header goes out: a 429 here is a clean status
    // code, not a truncated 200 the client has to recover from.
    if (!slots.try_acquire()) {
        send_error(res, 429, busy_message(slots));
        return;
    }

    auto st = std::make_shared<PcmStream>();
    const float  rate     = req.rate;
    const size_t text_len = utf8_length(body.text);

    st->th = std::thread([&ctx, &slots, st, req, voice, text_len, rate, t0]() {
        // The slot belongs to this thread from here on: the handler returns
        // before the stream finishes.
        struct Guard {
            Slots* slots;
            ~Guard() { slots->release(); }
        } slot_guard{&slots};

        double first_ms = 0;
        auto sink = [st, t0, &first_ms](const float* samples, int n) {
            if (st->client_gone.load(std::memory_order_acquire)) return false;
            if (n <= 0 || !samples) return true;
            if (first_ms <= 0) first_ms = ms_since(t0);

            std::string bytes(size_t(n) * 2, '\0');
            for (int i = 0; i < n; ++i) {
                const float f = std::max(-1.0f, std::min(1.0f, samples[i]));
                const int16_t v = int16_t(std::lround(f * 32767.0f));
                bytes[size_t(i) * 2 + 0] = char(uint8_t(v & 0xFF));
                bytes[size_t(i) * 2 + 1] = char(uint8_t((v >> 8) & 0xFF));
            }
            std::lock_guard<std::mutex> lk(st->mu);
            st->pending += bytes;
            st->samples += size_t(n);
            st->cv.notify_all();
            return true;
        };

        int sr = 0;
        Perf::Sample sample;
        sample.kind     = "pcm";
        sample.text_len = text_len;
        sample.rate     = rate;

        try {
            ctx.engine->synthesize_stream(voice, req, sink, &sr);
        } catch (const TimeoutError& e) {
            std::lock_guard<std::mutex> lk(st->mu);
            st->failed    = true;
            st->timed_out = true;
            st->err       = e.what();
            st->done      = true;
            st->cv.notify_all();
            sample.ok       = false;
            sample.total_ms = ms_since(t0);
            record_perf(ctx, &sample);
            return;
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(st->mu);
            st->failed = true;
            st->err    = e.what();
            st->done   = true;
            st->cv.notify_all();
            sample.ok       = false;
            sample.total_ms = ms_since(t0);
            record_perf(ctx, &sample);
            return;
        }
        std::lock_guard<std::mutex> lk(st->mu);
        st->done = true;
        st->cv.notify_all();
        sample.total_ms  = ms_since(t0);
        sample.first_ms  = first_ms;
        sample.audio_sec = double(st->samples) / double(kSampleRate);
        record_perf(ctx, &sample);
    });

    // Wait for the first audio or for a failure before committing to 200: that
    // is what lets a bad voice or OOM still answer with a real status code
    // instead of a truncated stream.
    {
        std::unique_lock<std::mutex> lk(st->mu);
        st->cv.wait(lk, [&] { return st->done || !st->pending.empty(); });
        if (st->pending.empty() && st->failed) {
            const std::string msg = st->err;
            const bool timed_out = st->timed_out;
            lk.unlock();
            st->th.join();
            if (timed_out) {
                send_error(res, 504, msg);
            } else {
                send_error(res, 500, "synthesis failed: " + msg);
            }
            return;
        }
    }

    res.set_header("Cache-Control", "no-cache");
    res.set_header("X-Accel-Buffering", "no");
    res.set_chunked_content_provider(
        "application/octet-stream",
        [st](size_t, httplib::DataSink& sink) -> bool {
            std::string chunk;
            bool done   = false;
            bool failed = false;
            {
                std::unique_lock<std::mutex> lk(st->mu);
                st->cv.wait(lk, [&] { return st->done || !st->pending.empty(); });
                chunk.swap(st->pending);
                done   = st->done;
                failed = st->failed;
            }

            std::string out;
            if (!st->header_sent) {
                // Emitted with the first payload block so the very first byte
                // the client sees is a valid frame header.
                out.append(kPcmMagic, 4);
                const uint8_t head[12] = {
                    1, 16, 1, 0,
                    uint8_t(kSampleRate & 0xFF), uint8_t((kSampleRate >> 8) & 0xFF),
                    uint8_t((kSampleRate >> 16) & 0xFF), uint8_t((kSampleRate >> 24) & 0xFF),
                    0, 0, 0, 0};
                out.append(reinterpret_cast<const char*>(head), sizeof(head));
                st->header_sent = true;
            }
            out += chunk;

            if (done && chunk.empty()) {
                // Nothing more is coming. Close cleanly only when the whole
                // utterance made it; otherwise leave QEND out so the client
                // discards what it received and retries through /tts.
                if (failed) return false;
                const uint32_t total = uint32_t(st->samples & 0xFFFFFFFF);
                std::string tail(kPcmEnd, 4);
                tail.push_back(char(uint8_t(total & 0xFF)));
                tail.push_back(char(uint8_t((total >> 8) & 0xFF)));
                tail.push_back(char(uint8_t((total >> 16) & 0xFF)));
                tail.push_back(char(uint8_t((total >> 24) & 0xFF)));
                if (!sink.write(tail.data(), tail.size())) {
                    st->client_gone.store(true, std::memory_order_release);
                    return false;
                }
                sink.done();
                return true;
            }

            if (!out.empty() && !sink.write(out.data(), out.size())) {
                st->client_gone.store(true, std::memory_order_release);
                return false;
            }
            return true;
        },
        [st](bool) {
            st->client_gone.store(true, std::memory_order_release);
            if (st->th.joinable()) st->th.join();
        });
}

// -----------------------------------------------------------------------------
//  /voices/new and /voices/clone
// -----------------------------------------------------------------------------
void handle_voice_new(ServerContext& ctx, const httplib::Request& http_req,
                      httplib::Response& res) {
    yyjson_doc* doc = yyjson_read(http_req.body.c_str(), http_req.body.size(), 0);
    if (!doc) {
        send_error(res, 400, "invalid JSON body");
        return;
    }
    yyjson_val* root = yyjson_doc_get_root(doc);
    std::string id, description, script, language;
    bool ok = yyjson_is_obj(root);
    ok = ok && get_str(root, "id", &id);
    ok = ok && get_str(root, "description", &description);
    ok = ok && get_str(root, "script", &script);
    ok = ok && get_str(root, "language", &language);
    yyjson_doc_free(doc);

    if (!ok) {
        send_error(res, 400, "invalid /voices/new body");
        return;
    }
    if (!VoiceStore::valid_id(id)) {
        send_error(res, 400, "illegal voice id");
        return;
    }
    if (trim(description).empty()) {
        send_error(res, 400, "description cannot be empty");
        return;
    }
    if (utf8_length(description) > 500) {
        send_error(res, 400, "description is longer than 500 characters");
        return;
    }
    if (ctx.voices->exists(id)) {
        send_error(res, 409, "voice '" + id + "' already exists");
        return;
    }
    if (trim(script).empty()) script = ctx.cfg.default_script;
    if (trim(script).empty()) {
        send_error(res, 400, "script is empty and default_script is not configured");
        return;
    }
    if (language.empty()) language = ctx.cfg.language;

    Perf::Sample sample;
    sample.kind = "design";
    const auto t0 = Clock::now();
    try {
        const AudioBuffer ref = ctx.engine->design(description, script, language);
        const Voice v = ctx.voices->add_designed(id, description, script, language, ref);
        sample.total_ms  = ms_since(t0);
        sample.audio_sec = ref.sample_rate > 0
                               ? double(ref.samples.size()) / double(ref.sample_rate)
                               : 0;
        record_perf(ctx, &sample);
        ctx.engine->drop_voice(id);   // a recycled id must not reuse latents
        send_json(res, 200, voice_json(v, ctx.voices->ref_path(v.id)));
    } catch (const TimeoutError& e) {
        sample.total_ms = ms_since(t0);
        sample.ok       = false;
        record_perf(ctx, &sample);
        send_error(res, 504, e.what());
    } catch (const std::exception& e) {
        sample.total_ms = ms_since(t0);
        sample.ok       = false;
        record_perf(ctx, &sample);
        send_error(res, 500, std::string("voice design failed: ") + e.what());
    }
}

void handle_voice_clone(ServerContext& ctx, const httplib::Request& http_req,
                        httplib::Response& res) {
    yyjson_doc* doc = yyjson_read(http_req.body.c_str(), http_req.body.size(), 0);
    if (!doc) {
        send_error(res, 400, "invalid JSON body");
        return;
    }
    yyjson_val* root = yyjson_doc_get_root(doc);
    std::string id, b64, ref_text, language;
    bool ok = yyjson_is_obj(root);
    ok = ok && get_str(root, "id", &id);
    ok = ok && get_str(root, "ref_audio_base64", &b64);
    ok = ok && get_str(root, "ref_text", &ref_text);
    ok = ok && get_str(root, "language", &language);
    yyjson_doc_free(doc);

    if (!ok) {
        send_error(res, 400, "invalid /voices/clone body");
        return;
    }
    if (!VoiceStore::valid_id(id)) {
        send_error(res, 400, "illegal voice id");
        return;
    }
    if (ctx.voices->exists(id)) {
        send_error(res, 409, "voice '" + id + "' already exists");
        return;
    }
    if (b64.empty()) {
        send_error(res, 400, "ref_audio_base64 is empty");
        return;
    }
    if (b64.size() > kMaxRefB64Chars) {
        send_error(res, 400, "reference audio too large (base64 over 28,000,000 chars)");
        return;
    }

    std::string raw;
    if (!base64_decode(b64, &raw)) {
        send_error(res, 400, "reference audio is not valid base64");
        return;
    }
    AudioBuffer ref;
    try {
        ref = wav_decode(reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
    } catch (const std::exception& e) {
        send_error(res, 400, std::string("cannot decode reference audio: ") + e.what());
        return;
    }
    if (ref.sample_rate != kSampleRate) ref = resample_linear(ref, kSampleRate);
    if (ref.samples.empty()) {
        send_error(res, 400, "reference audio is empty");
        return;
    }
    if (double(ref.samples.size()) / double(ref.sample_rate) > kMaxRefSeconds) {
        send_error(res, 400, "reference audio is longer than 90 s");
        return;
    }
    if (utf8_length(ref_text) > 1000) {
        send_error(res, 400, "ref_text is longer than 1000 characters");
        return;
    }
    if (language.empty()) language = ctx.cfg.language;

    try {
        const Voice v = ctx.voices->add_cloned(id, ref, ref_text, language);
        ctx.engine->drop_voice(id);
        send_json(res, 200, voice_json(v, ctx.voices->ref_path(v.id)));
    } catch (const std::exception& e) {
        send_error(res, 400, std::string("cannot register voice: ") + e.what());
    }
}

}  // namespace

// -----------------------------------------------------------------------------
//  server
// -----------------------------------------------------------------------------
void run_http_server(ServerContext& ctx) {
    if (!ctx.voices || !ctx.engine) {
        throw std::runtime_error("http: ServerContext is missing the voice store or engine");
    }

    Slots slots(ctx.cfg.max_queue);

    httplib::Server svr;
    svr.set_read_timeout(60);
    svr.set_write_timeout(120);
    // A clone request carries up to ~20 MB of base64 audio plus JSON overhead.
    svr.set_payload_max_length(kPayloadMax);
    svr.set_tcp_nodelay(true);
    svr.set_default_headers({
        {"Cache-Control", "no-store"},
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type"},
    });

    svr.Options(R"(.*)", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });

    // -- health ---------------------------------------------------------------
    svr.Get("/health", [&ctx, &slots](const httplib::Request&, httplib::Response& res) {
        try {
            std::string o = "{";
            o += "\"status\":\"ok\",";
            o += "\"version\":\"" + std::string(kServerVersion) + "\",";
            o += "\"ready\":true,";
            o += "\"started_at\":\"" + json_escape(ctx.started_at) + "\",";
            o += "\"models_root\":\"" + json_escape(ctx.cfg.models_root) + "\",";
            o += "\"device\":\"" +
                 json_escape(ctx.cfg.device.empty() ? "auto" : ctx.cfg.device) + "\",";
            o += "\"dtype\":\"gguf\",";
            o += "\"backend\":\"qwentts.cpp(ggml)\",";
            o += "\"base_loaded\":" +
                 std::string(ctx.engine->base_loaded() ? "true" : "false") + ",";
            o += "\"design_loaded\":" +
                 std::string(ctx.engine->design_loaded() ? "true" : "false") + ",";
            o += "\"base_missing\":\"\",";
            o += "\"design_missing\":\"\",";
            o += "\"voices\":" + std::to_string(ctx.voices->list().size()) + ",";
            // The game only starts using POST /tts/pcm when it sees this key.
            o += "\"stream_protocol\":\"" +
                 std::string(ctx.cfg.stream ? kPcmProtocol : "") + "\",";
            o += "\"stream_chunk\":" + std::to_string(ctx.cfg.stream_chunk) + ",";
            o += "\"max_queue\":" + std::to_string(slots.limit()) + ",";
            o += "\"inflight\":" + std::to_string(ctx.engine->inflight()) + ",";
            o += "\"engine\":\"" + json_escape(ctx.engine->describe()) + "\"";
            o += "}";
            send_json(res, 200, o);
        } catch (const std::exception& e) {
            send_error(res, 500, e.what());
        }
    });

    // -- perf -----------------------------------------------------------------
    svr.Get("/perf", [&ctx, &slots](const httplib::Request&, httplib::Response& res) {
        if (!ctx.perf) {
            send_error(res, 404, "perf disabled");
            return;
        }
        // The legacy payload also carried a gpu_mem snapshot. We do not link a
        // CUDA query into the service layer, so report the queue instead.
        std::string o = ctx.perf->json_summary();
        o.pop_back();   // drop the closing brace of the Perf payload
        o += ",\"inflight\":" + std::to_string(ctx.engine->inflight());
        o += ",\"max_queue\":" + std::to_string(slots.limit());
        o += "}";
        send_json(res, 200, o);
    });

    // -- voices ---------------------------------------------------------------
    svr.Get("/voices", [&ctx](const httplib::Request&, httplib::Response& res) {
        const auto  all = ctx.voices->list();
        std::string o   = "[";
        bool        first = true;
        for (const auto& v : all) {
            if (!first) o += ",";
            first = false;
            o += voice_json(v, ctx.voices->ref_path(v.id));
        }
        o += "]";
        send_json(res, 200, o);
    });

    svr.Post("/voices/new", [&ctx](const httplib::Request& req, httplib::Response& res) {
        handle_voice_new(ctx, req, res);
    });

    svr.Post("/voices/clone", [&ctx](const httplib::Request& req, httplib::Response& res) {
        handle_voice_clone(ctx, req, res);
    });

    svr.Delete(R"(/voices/([A-Za-z0-9][A-Za-z0-9_.-]{0,63}))",
               [&ctx](const httplib::Request& http_req, httplib::Response& res) {
                   const std::string id = http_req.matches[1];
                   if (!VoiceStore::valid_id(id)) {
                       send_error(res, 400, "illegal voice id");
                       return;
                   }
                   if (!ctx.voices->erase(id)) {
                       send_error(res, 404, "voice '" + id + "' does not exist");
                       return;
                   }
                   ctx.engine->drop_voice(id);
                   send_json(res, 200, "{\"deleted\":\"" + json_escape(id) + "\"}");
               });

    // -- synthesis ------------------------------------------------------------
    svr.Post("/tts", [&ctx, &slots](const httplib::Request& req, httplib::Response& res) {
        handle_tts(ctx, slots, req, res);
    });

    svr.Post("/tts/pcm", [&ctx, &slots](const httplib::Request& req,
                                        httplib::Response& res) {
        handle_tts_pcm(ctx, slots, req, res);
    });

    const std::string where = ctx.cfg.host + ":" + std::to_string(ctx.cfg.port);
    // Bind first so a busy port is reported as an error instead of a thread
    // that silently never becomes ready. Splitting listen() lets us publish the
    // stop hook and fire on_listening only once the socket actually accepts.
    if (!svr.bind_to_port(ctx.cfg.host.c_str(), ctx.cfg.port)) {
        throw std::runtime_error("cannot listen on " + where);
    }
    ctx.request_stop = [&svr]() { svr.stop(); };
    std::fprintf(stderr, "[ttsc] listening on http://%s\n", where.c_str());
    if (ctx.on_listening) ctx.on_listening();
    svr.listen_after_bind();
    ctx.request_stop = nullptr;
}

}  // namespace ttsc
