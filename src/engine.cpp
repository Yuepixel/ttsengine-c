// =============================================================================
//  engine.cpp - adapter between the legacy server contract and qwentts.cpp
//
//  What this layer adds on top of the vendored C ABI
//    1. Two contexts. Base (speak / clone) is loaded at start; VoiceDesign
//       (mint a voice from a text description) is loaded lazily and released
//       right after use unless keep_design_loaded is set, because keeping both
//       talkers resident costs ~2.5 GB on a 16 GB card.
//    2. Voice latents. The ABI wants a speaker embedding plus, for ICL mode,
//       an RVQ code matrix. Encoding them from the reference WAV costs
//       ~0.2-1 s, so they are cached in the voice store (ref.spk / ref.rvq)
//       and regenerated only on a miss.
//    3. Speaking rate. The model has no rate knob; the legacy server applied
//       WSOLA after synthesis, so we do the same: 24 kHz mono in, 24 kHz mono
//       out, pitch preserved.
//    4. Warm-up. The first synthesis of a process pays ~3 s of one-off cost
//       (CUDA module load, graph capture, clock ramp). preload() eats it with
//       a short utterance so the first real request is not penalised.
//
//  Threading
//    qt_synthesize is thread safe and, with max_batch > 1, internally batches
//    concurrent calls on a single compute worker. The ABI invokes on_chunk on
//    that worker, never on the caller thread, so the trampoline below only
//    copies into our own buffer / forwards to the caller's sink and never
//    calls back into the qwen_* API.
// =============================================================================
#include "ttsc.h"

#include "qwen.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>

namespace ttsc {
namespace {

constexpr int   kSampleRate   = 24000;   // fixed by the 12 Hz codec
constexpr float kRateMin      = 1.0f;    // never stretch *down*: the MOD's own floor is
                                        // 1.0 and every slow-down variant was judged worse
                                        // than the unmodified waveform (2026-10-20 A/B). An
                                        // old MOD build still asks for 0.96 for slow pawns,
                                        // which used to come back sounding "像方波".
constexpr float kRateMax      = 2.0f;
constexpr int   kEmitBlock    = 4096;    // ~170 ms per callback on the stretch path
// Short filler used only to warm the GPU path. Kept tiny so start-up stays
// snappy; it still pays the codec graph capture that dominated the cold run.
const char* kWarmupText = "你好，这是一次预热。";

std::string lower_ascii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    }
    return s;
}

// qt_log_set is process global; route it to stderr and honour the debug flag.
// A plain fprintf is intentional: the ABI can log from any thread and this is
// the one place that must not allocate or lock.
void log_trampoline(qt_log_level level, const char* msg, void* user_data) {
    const bool debug = user_data != nullptr && *static_cast<const bool*>(user_data);
    if (!debug && level < QT_LOG_WARN) return;
    const char* tag = level >= QT_LOG_ERROR ? "error"
                    : level == QT_LOG_WARN  ? "warn"
                    : level == QT_LOG_INFO  ? "info"
                                            : "debug";
    std::fprintf(stderr, "[qwen/%s] %s\n", tag, msg);
    std::fflush(stderr);
}

double now_ms() {
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - t0).count();
}

// Per-call state shared with the ABI callbacks. The ABI runs them on its own
// compute worker, so every field they touch is either a pointer to storage
// that outlives the call (Engine::Impl members guarded by reference) or an
// atomic owned by this struct on the caller's stack.
struct CallState {
    const std::function<bool(const float*, int)>* sink = nullptr;
    std::atomic<bool>                            cancel{false};
    std::atomic<bool>                            timed_out{false};
    // Wall-clock budget for this call (zero = unlimited). Checked from the
    // callbacks rather than a watchdog thread: the ABI polls `cancel` on every
    // decode step, so the timeout takes effect within a few milliseconds and
    // needs no extra thread.
    std::chrono::steady_clock::time_point        deadline{};
    std::vector<float>                           acc;   // stretch-in-stream buffer
};

bool deadline_passed(CallState* st) {
    if (st->deadline == std::chrono::steady_clock::time_point{}) return false;
    if (std::chrono::steady_clock::now() < st->deadline) return false;
    st->timed_out.store(true, std::memory_order_relaxed);
    return true;
}

bool cancel_cb(void* ud) {
    auto* st = static_cast<CallState*>(ud);
    return st->cancel.load(std::memory_order_relaxed) || deadline_passed(st);
}

// Forwarding trampoline. On the streaming path the sink is the caller's
// function; returning false propagates back as a cancellation.
bool chunk_cb(const float* samples, int n, void* ud) {
    auto* st = static_cast<CallState*>(ud);
    if (st->cancel.load(std::memory_order_relaxed) || deadline_passed(st)) return false;
    if (!st->sink) return true;
    return (*st->sink)(samples, n);
}

// Used only when stream_stretch is enabled: collect the utterance instead of
// emitting it, so WSOLA can splice the whole waveform afterwards.
bool accumulate_cb(const float* samples, int n, void* ud) {
    auto* st = static_cast<CallState*>(ud);
    if (st->cancel.load(std::memory_order_relaxed) || deadline_passed(st)) return false;
    if (n > 0 && samples) st->acc.insert(st->acc.end(), samples, samples + n);
    return true;
}

// Convert the ABI voice ref into our owning struct. Both buffers are copied
// because qt_voice_ref_free() releases them when the call returns.
VoiceRef copy_ref(const qt_voice_ref& src) {
    VoiceRef out;
    if (src.ref_spk_emb && src.ref_spk_dim > 0) {
        out.spk.assign(src.ref_spk_emb, src.ref_spk_emb + src.ref_spk_dim);
    }
    if (src.ref_codes && src.ref_T > 0 && src.num_codebooks > 0) {
        const size_t n = size_t(src.num_codebooks) * size_t(src.ref_T);
        out.codes.assign(src.ref_codes, src.ref_codes + n);
        out.codebooks = src.num_codebooks;
        out.frames    = src.ref_T;
    }
    return out;
}

}  // namespace

// -----------------------------------------------------------------------------
//  Impl
// -----------------------------------------------------------------------------
struct Engine::Impl {
    Config      cfg;
    VoiceStore* voices = nullptr;

    // Talkers. base_ lives for the whole process; design_ is transient unless
    // keep_design_loaded is set.
    std::mutex   ctx_mu;
    qt_context*  base = nullptr;

    mutable std::mutex design_mu;
    qt_context*        design = nullptr;
    bool               design_live = false;

    // Latent cache. Keyed by voice id. Invalidate by restarting or by asking
    // the store for fresh latents (see ensure_ref); the reference audio of a
    // voice is immutable once created, so a stale entry cannot happen through
    // the normal API.
    std::mutex               ref_mu;
    std::map<std::string, VoiceRef> ref_cache;

    std::atomic<int> inflight{0};
    bool             debug = false;   // referenced by log_trampoline

    // ---- context helpers ----------------------------------------------------
    qt_context* init_ctx(const std::string& talker_path, const char* what) {
        qt_init_params p;
        qt_init_default_params(&p);
        p.talker_path = talker_path.c_str();
        p.codec_path  = cfg.codec_gguf.c_str();
        p.use_fa      = cfg.use_fa;
        p.max_batch   = cfg.max_batch > 0 ? cfg.max_batch : 1;
        p.device      = cfg.device.empty() ? nullptr : cfg.device.c_str();
        // codec_chunk_sec stays at the ABI default (24 s = whole utterance for
        // anything short) because splitting the decode adds a redecode of the
        // context per chunk and buys nothing at 16 GB.

        qt_context* q = qt_init(&p);
        if (!q) {
            throw std::runtime_error(std::string("load ") + what + " model failed: " +
                                     talker_path + ": " + qt_last_error());
        }
        return q;
    }

    qt_context* ensure_base() {
        std::lock_guard<std::mutex> lk(ctx_mu);
        if (!base) base = init_ctx(cfg.base_gguf, "base");
        return base;
    }

    // ---- language -----------------------------------------------------------
    // qt_tts_params.lang wants the lowercase names from qt_language_name, or
    // NULL for "let the model infer it". Anything unknown falls back to auto
    // rather than failing a request over a cosmetic config value.
    std::string resolve_lang(qt_context* q, const std::string& wanted) const {
        const std::string want = lower_ascii(trim(wanted));
        if (want.empty() || want == "auto") return {};
        const int n = qt_n_languages(q);
        for (int i = 0; i < n; ++i) {
            const char* name = qt_language_name(q, i);
            if (name && want == name) return want;
        }
        return {};
    }

    // ---- voice reference ----------------------------------------------------
    // Cache hit -> cached latents. Miss -> load ref.spk/ref.rvq from the store,
    // and if those are missing encode ref.wav once with the speaker encoder +
    // codec encoder, then persist them for the next process.
    VoiceRef ensure_ref(const Voice& v) {
        {
            std::lock_guard<std::mutex> lk(ref_mu);
            const auto it = ref_cache.find(v.id);
            if (it != ref_cache.end()) return it->second;
        }

        VoiceRef ref;
        bool have = voices && voices->load_latents(v.id, &ref);

        if (!have) {
            const std::string wav = voices ? voices->ref_path(v.id) : std::string();
            if (wav.empty() || !path_exists(wav)) {
                throw std::runtime_error("voice '" + v.id + "' has no reference audio");
            }
            std::ifstream f(wav, std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                       std::istreambuf_iterator<char>());
            if (bytes.empty()) {
                throw std::runtime_error("voice '" + v.id + "': empty reference audio");
            }
            AudioBuffer audio = wav_decode(bytes.data(), bytes.size());
            if (audio.sample_rate != kSampleRate) {
                audio = resample_linear(audio, kSampleRate);
            }
            if (audio.samples.empty()) {
                throw std::runtime_error("voice '" + v.id + "': reference audio is silent");
            }

            qt_context* q = ensure_base();
            qt_voice_ref raw = {};
            const qt_status rc = qt_extract_voice_ref(q, audio.samples.data(),
                                                      int(audio.samples.size()), &raw);
            if (rc != QT_STATUS_OK) {
                throw std::runtime_error("voice '" + v.id +
                                         "': reference encode failed: " + qt_last_error());
            }
            ref = copy_ref(raw);
            qt_voice_ref_free(&raw);
            if (voices) voices->save_latents(v.id, ref);
        }

        {
            std::lock_guard<std::mutex> lk(ref_mu);
            ref_cache[v.id] = ref;   // copy: a concurrent miss may have won
            return ref_cache[v.id];
        }
    }

    // ---- synthesis ----------------------------------------------------------
    // Fills `p` with the sampling defaults from the config plus the request
    // overrides. `keep` must outlive the qt_synthesize call: it holds the
    // strings whose c_str() pointers are stored in `p`.
    struct StrKeep {
        std::string text, lang, instruct, ref_text;
        VoiceRef    ref;   // owns the latent buffers the params point into
    };

    void fill_params(qt_tts_params* p, const Voice& v, const Request& req,
                     StrKeep* keep, CallState* st, bool streaming) {
        qt_tts_default_params(p);   // sets abi_version and sampling defaults

        keep->text    = req.text;
        keep->lang    = req.language.empty() ? cfg.language : req.language;
        // The base checkpoint rejects instruct outright (qwen.cpp:663), so it
        // is only forwarded to the design context by design().
        keep->ref_text = !v.ref_text.empty() ? v.ref_text : v.script;

        p->text        = keep->text.c_str();
        // Decoding budget. Upstream defaults to 2048 frames = 164 s of audio,
        // so a degenerate repetition loop occupies the GPU for ~3 minutes and
        // starves every queued request. The Python server capped this per
        // request at 6 frames per character + 48, clamped to [32, 1024]; we
        // keep the same rule so both servers fail in the same way.
        {
            int tokens = cfg.max_new_tokens;
            if (tokens <= 0) {
                size_t chars = 0;
                for (unsigned char c : keep->text) {
                    if ((c & 0xC0) != 0x80) ++chars;
                }
                tokens = int(chars) * 6 + 48;
                tokens = std::max(32, std::min(1024, tokens));
            }
            p->max_new_tokens = tokens;
        }
        p->temperature = req.temperature > 0 ? req.temperature : cfg.temperature;
        p->top_k       = req.top_k > 0 ? req.top_k : cfg.top_k;
        p->top_p       = req.top_p > 0 ? req.top_p : cfg.top_p;
        p->repetition_penalty =
            req.repetition_penalty > 0 ? req.repetition_penalty : cfg.repetition_penalty;

        p->cancel           = cancel_cb;
        p->cancel_user_data = st;
        if (cfg.synth_timeout_ms > 0) {
            st->deadline = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(cfg.synth_timeout_ms);
        }
        if (streaming) {
            // No sink means the caller wants the utterance collected first (the
            // stream_stretch path); otherwise forward chunk by chunk.
            p->on_chunk           = st->sink ? chunk_cb : accumulate_cb;
            p->on_chunk_user_data = st;
        }

        // Talker handle is needed to validate / translate the language name.
        qt_context* q = ensure_base();
        keep->lang = resolve_lang(q, keep->lang);
        if (!keep->lang.empty()) p->lang = keep->lang.c_str();

        // Voice conditioning. x_vector_only maps to clone mode A (speaker
        // embedding alone); otherwise ICL mode B adds the RVQ codes and needs
        // the transcript that was spoken in the reference.
        keep->ref = ensure_ref(v);
        const VoiceRef& ref = keep->ref;
        if (!ref.spk.empty()) {
            p->ref_spk_emb = ref.spk.data();
            p->ref_spk_dim = int(ref.spk.size());
            if (!v.x_vector_only && ref.has_codes() && !keep->ref_text.empty()) {
                p->ref_codes = ref.codes.data();
                p->ref_T     = ref.frames;
                p->ref_text  = keep->ref_text.c_str();
            }
        }
    }

    // Emitted only when stream_stretch is on: the caller gets the stretched
    // waveform in ~170 ms blocks after synthesis finishes. Kept implemented on
    // purpose — refusing a rate-changed stream by default is a debugging and
    // wire-compatibility policy (see Config::stream_stretch), not a limit of
    // this engine.
    void emit_stretched(const std::vector<float>& pcm, float rate,
                        const std::function<bool(const float*, int)>& sink,
                        std::atomic<bool>* cancel) {
        AudioBuffer in;
        in.sample_rate = kSampleRate;
        in.samples     = pcm;
        const AudioBuffer out = wsola::stretch(in, rate);
        const int n = int(out.samples.size());
        for (int off = 0; off < n; off += kEmitBlock) {
            if (cancel->load(std::memory_order_relaxed)) return;
            const int len = std::min(kEmitBlock, n - off);
            if (!sink(out.samples.data() + off, len)) return;
        }
    }
};

// -----------------------------------------------------------------------------
//  Engine
// -----------------------------------------------------------------------------
Engine::Engine(Config cfg, VoiceStore* voices) : impl_(new Impl()) {
    impl_->cfg    = std::move(cfg);
    impl_->voices = voices;
    impl_->debug  = impl_->cfg.debug;
    qt_log_set(log_trampoline, &impl_->debug);
}

Engine::~Engine() {
    // Order matters: release the design handle first (it may share the codec
    // file handle), then the base one.
    if (impl_) {
        qt_log_set(nullptr, nullptr);   // user_data points into impl_
        if (impl_->design) qt_free(impl_->design);
        if (impl_->base) qt_free(impl_->base);
        delete impl_;
        impl_ = nullptr;
    }
}

void Engine::preload() {
    impl_->ensure_base();

    // Warm-up. Uses a real voice when one exists so the exact code path of a
    // production request (latents -> prompt -> decode) is exercised; otherwise
    // a bare text-only run still pays the CUDA module load and graph capture.
    const double t0 = now_ms();
    std::string  used = "(text-only)";
    try {
        Voice     v;
        bool      have = false;
        if (impl_->voices) {
            const auto all = impl_->voices->list();
            if (!all.empty()) {
                v    = all.front();
                have = true;
                used = v.id;
            }
        }
        if (!have) {
            // ensure_ref would throw for a voice with no audio, so the
            // text-only path bypasses fill_params' voice handling.
            qt_tts_params p;
            qt_tts_default_params(&p);
            const std::string lang = impl_->resolve_lang(impl_->base, impl_->cfg.language);
            p.text = kWarmupText;
            p.lang = lang.empty() ? nullptr : lang.c_str();
            p.max_new_tokens = 64;
            qt_audio audio = {};
            const qt_status rc = qt_synthesize(impl_->base, &p, &audio);
            qt_audio_free(&audio);
            if (rc != QT_STATUS_OK) {
                std::fprintf(stderr, "[ttsc] warmup (text-only) failed: %s\n", qt_last_error());
            }
        } else {
            Request req;
            req.text     = kWarmupText;
            req.language = v.language;
            AudioBuffer out;
            synthesize(v, req, &out);
        }
    } catch (const std::exception& e) {
        // A failed warm-up must not stop the server: the first request will
        // simply pay the cold cost again.
        std::fprintf(stderr, "[ttsc] warmup failed: %s\n", e.what());
    }
    std::fprintf(stderr, "[ttsc] warmup done in %.0f ms (%s)\n", now_ms() - t0, used.c_str());
}

bool Engine::base_loaded() const {
    std::lock_guard<std::mutex> lk(impl_->ctx_mu);
    return impl_->base != nullptr;
}

bool Engine::design_loaded() const {
    std::lock_guard<std::mutex> lk(impl_->design_mu);
    return impl_->design_live;
}

void Engine::set_keep_design_loaded(bool keep) {
    impl_->cfg.keep_design_loaded = keep;
}

void Engine::synthesize(const Voice& voice, const Request& req, AudioBuffer* out) {
    if (!out) throw std::runtime_error("synthesize: null output");
    if (trim(req.text).empty()) throw std::runtime_error("text is empty");

    impl_->inflight.fetch_add(1);
    struct Guard {
        std::atomic<int>* c;
        ~Guard() { c->fetch_sub(1); }
    } guard{&impl_->inflight};

    Impl::StrKeep     keep;
    CallState         st;
    qt_tts_params     p;
    impl_->fill_params(&p, voice, req, &keep, &st, /*streaming=*/false);

    qt_audio audio = {};
    const qt_status rc = qt_synthesize(impl_->ensure_base(), &p, &audio);
    if (rc != QT_STATUS_OK) {
        const std::string err = qt_last_error();
        qt_audio_free(&audio);
        if (st.timed_out.load(std::memory_order_relaxed)) {
            throw TimeoutError("synthesis exceeded " +
                               std::to_string(impl_->cfg.synth_timeout_ms) + " ms");
        }
        throw std::runtime_error(err.empty() ? "synthesis failed" : err);
    }

    AudioBuffer buf;
    buf.sample_rate = audio.sample_rate > 0 ? audio.sample_rate : kSampleRate;
    if (audio.samples && audio.n_samples > 0) {
        buf.samples.assign(audio.samples, audio.samples + audio.n_samples);
    }
    qt_audio_free(&audio);

    float rate = req.rate;
    if (!(rate > 0.0f)) rate = 1.0f;
    rate = std::max(kRateMin, std::min(kRateMax, rate));
    if (impl_->cfg.time_stretch && std::fabs(rate - 1.0f) >= 0.001f) {
        buf = wsola::stretch(buf, rate);
    }
    *out = std::move(buf);
}

void Engine::synthesize_stream(const Voice& voice, const Request& req,
                               const std::function<bool(const float*, int)>& on_chunk,
                               int* out_sample_rate) {
    if (!on_chunk) throw std::runtime_error("synthesize_stream: no callback");
    if (trim(req.text).empty()) throw std::runtime_error("text is empty");
    if (!stream_usable(req)) {
        // Deliberate policy, not a limitation: the stretch path below works,
        // it is just disabled by default so a rate-changed line always takes
        // the buffered /tts route (see Config::stream_stretch).
        throw std::runtime_error(
            "streaming refused for rate change by policy (stream_stretch=0); "
            "the engine supports it, the caller should use buffered /tts");
    }

    impl_->inflight.fetch_add(1);
    struct Guard {
        std::atomic<int>* c;
        ~Guard() { c->fetch_sub(1); }
    } guard{&impl_->inflight};

    float rate = req.rate;
    if (!(rate > 0.0f)) rate = 1.0f;
    rate = std::max(kRateMin, std::min(kRateMax, rate));
    const bool stretch = impl_->cfg.time_stretch && std::fabs(rate - 1.0f) > 0.03f;

    Impl::StrKeep keep;
    CallState     st;
    qt_tts_params p;
    // With stretching enabled the ABI fills our accumulator; the stretched
    // result is replayed through the caller's sink afterwards.
    st.sink = stretch ? nullptr : &on_chunk;
    impl_->fill_params(&p, voice, req, &keep, &st, /*streaming=*/true);

    qt_audio audio = {};
    const qt_status rc = qt_synthesize(impl_->ensure_base(), &p, &audio);
    if (rc != QT_STATUS_OK) {
        const std::string err = qt_last_error();
        qt_audio_free(&audio);
        if (st.timed_out.load(std::memory_order_relaxed)) {
            throw TimeoutError("streaming exceeded " +
                               std::to_string(impl_->cfg.synth_timeout_ms) + " ms");
        }
        if (rc == QT_STATUS_CANCELLED) return;
        throw std::runtime_error(err.empty() ? "synthesis failed" : err);
    }
    // Streaming mode leaves `out` empty by contract; free defensively anyway.
    qt_audio_free(&audio);

    if (out_sample_rate) *out_sample_rate = kSampleRate;
    if (stretch) impl_->emit_stretched(st.acc, rate, on_chunk, &st.cancel);
}

bool Engine::stream_usable(const Request& req) const {
    // Mirrors legacy engine.stream_usable(), with one addition: when
    // time_stretch is off the rate is ignored entirely, so streaming is
    // always fine; when stream_stretch is on we serve the stretched stream
    // ourselves, so any rate is fine too. Otherwise a rate outside the 3 %
    // window is refused — deliberately, to keep MOD traffic on one transport
    // per request while both servers are being A/B'd. Not a capability limit.
    if (!impl_->cfg.time_stretch) return true;
    if (impl_->cfg.stream_stretch) return true;
    const float rate = req.rate > 0.0f ? req.rate : 1.0f;
    return std::fabs(rate - 1.0f) <= 0.03f;
}

AudioBuffer Engine::design(const std::string& description, const std::string& script,
                           const std::string& language) {
    if (trim(description).empty()) throw std::runtime_error("description is empty");
    if (trim(script).empty()) throw std::runtime_error("script is empty");

    impl_->inflight.fetch_add(1);
    struct Guard {
        std::atomic<int>* c;
        ~Guard() { c->fetch_sub(1); }
    } guard{&impl_->inflight};

    std::lock_guard<std::mutex> lk(impl_->design_mu);
    if (!impl_->design) {
        impl_->design      = impl_->init_ctx(impl_->cfg.design_gguf, "voice-design");
        impl_->design_live = true;
    }

    Impl::StrKeep keep;
    CallState     st;
    qt_tts_params p;
    qt_tts_default_params(&p);
    keep.text     = script;
    keep.instruct = description;
    keep.lang     = impl_->resolve_lang(impl_->design,
                                        language.empty() ? impl_->cfg.language : language);
    p.text     = keep.text.c_str();
    p.instruct = keep.instruct.c_str();
    p.lang     = keep.lang.empty() ? nullptr : keep.lang.c_str();
    p.temperature = impl_->cfg.temperature;
    p.top_k       = impl_->cfg.top_k;
    p.top_p       = impl_->cfg.top_p;
    p.repetition_penalty = impl_->cfg.repetition_penalty;
    p.cancel           = cancel_cb;
    p.cancel_user_data = &st;
    if (impl_->cfg.synth_timeout_ms > 0) {
        st.deadline = std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(impl_->cfg.synth_timeout_ms);
    }
    if (impl_->cfg.max_new_tokens > 0) p.max_new_tokens = impl_->cfg.max_new_tokens;

    qt_audio audio = {};
    const qt_status rc = qt_synthesize(impl_->design, &p, &audio);
    if (rc != QT_STATUS_OK) {
        const std::string err = qt_last_error();
        qt_audio_free(&audio);
        if (!impl_->cfg.keep_design_loaded) {
            qt_free(impl_->design);
            impl_->design      = nullptr;
            impl_->design_live = false;
        }
        if (st.timed_out.load(std::memory_order_relaxed)) {
            throw TimeoutError("voice design exceeded " +
                               std::to_string(impl_->cfg.synth_timeout_ms) + " ms");
        }
        throw std::runtime_error(err.empty() ? "voice design failed" : err);
    }

    AudioBuffer buf;
    buf.sample_rate = audio.sample_rate > 0 ? audio.sample_rate : kSampleRate;
    if (audio.samples && audio.n_samples > 0) {
        buf.samples.assign(audio.samples, audio.samples + audio.n_samples);
    }
    qt_audio_free(&audio);

    // Release the design talker unless the operator wants it pinned. Both
    // talkers resident is ~2.5 GB extra and only pays off when minting many
    // voices in a row.
    if (!impl_->cfg.keep_design_loaded) {
        qt_free(impl_->design);
        impl_->design      = nullptr;
        impl_->design_live = false;
    }
    return buf;
}

void Engine::drop_voice(const std::string& id) {
    std::lock_guard<std::mutex> lk(impl_->ref_mu);
    impl_->ref_cache.erase(id);
}

std::string Engine::describe() const {
    std::lock_guard<std::mutex> lk(impl_->ctx_mu);
    std::string s = "qwentts.cpp";
    if (impl_->base) {
        const char* mt = qt_model_type(impl_->base);
        if (mt && *mt) s += std::string("/") + mt;
    }
    s += " device=" + (impl_->cfg.device.empty() ? std::string("auto") : impl_->cfg.device);
    s += " max_batch=" + std::to_string(impl_->cfg.max_batch);
    return s;
}

int Engine::inflight() const {
    return impl_->inflight.load(std::memory_order_relaxed);
}

}  // namespace ttsc
