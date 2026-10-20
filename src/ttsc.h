// =============================================================================
//  ttsc.h - public declarations of the TtsEngine-C service layer
//
//  Scope
//    This header describes everything *we* own: configuration, WAV I/O,
//    time stretching, the voice registry, the engine wrapper around the
//    vendored qwen3-tts ABI, and the HTTP surface. The inference itself
//    lives in third_party/qwentts.cpp and is not exposed here.
//
//  Naming
//    Types are PascalCase, functions lower_snake_case, members trailing
//    underscore only for private state. Everything lives in namespace ttsc.
// =============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace ttsc {

// Thrown when a synthesis exceeds Config::synth_timeout_ms. The HTTP layer maps
// it to 504 so a degenerate repetition loop is reported as a timeout rather
// than as an engine fault.
class TimeoutError : public std::runtime_error {
public:
    explicit TimeoutError(const std::string& what) : std::runtime_error(what) {}
};

// -----------------------------------------------------------------------------
//  Config - key-compatible with the legacy Python server's ttsserver.ini
// -----------------------------------------------------------------------------
//  The legacy file is a flat `key = value` INI with `#` comments. Keeping the
//  same keys means an existing deployment can point the new binary at the same
//  ini file; unknown keys are ignored (and reported when debug is on).
struct Config {
    // -- filesystem -----------------------------------------------------------
    std::string models_root;   // directory holding the .gguf files
    std::string base_gguf;     // talker weights used for synthesis / cloning
    std::string design_gguf;   // talker weights used for VoiceDesign
    std::string codec_gguf;    // 12 Hz audio codec, shared by both talkers
    std::string data_dir;      // voices.json and cached reference audio

    // -- network --------------------------------------------------------------
    std::string host = "127.0.0.1";
    int         port = 8080;

    // -- engine ---------------------------------------------------------------
    std::string device;              // "CUDA0" / "CPU" / empty = best available
    int         max_batch = 1;       // concurrent sequences decoded per context
    bool        use_fa = true;       // fused flash attention
    bool        keep_design_loaded = false;

    // -- behaviour ------------------------------------------------------------
    bool stream = true;              // enable POST /tts/pcm
    int  stream_chunk = 12;          // legacy knob, kept for compatibility
    // Allow /tts/pcm when the request also needs a rate change. The engine can
    // do it (buffer, WSOLA, replay in ~170 ms blocks), but it costs the whole
    // synthesis before the first chunk, so it is OFF by default: during MOD
    // debugging we want a rate-changed request to take the buffered /tts path
    // every time instead of silently switching transport. This is a policy
    // switch, not a capability limit.
    bool stream_stretch = false;
    bool time_stretch = true;        // WSOLA rate control
    bool perf = true;                // per-request timing + GET /perf
    bool debug = false;
    int  max_queue = 2;              // concurrent synthesis slots
    int  synth_timeout_ms = 60000;

    // -- synthesis defaults ---------------------------------------------------
    std::string language = "Chinese";
    float       temperature = 0.9f;
    int         top_k = 50;
    float       top_p = 1.0f;
    float       repetition_penalty = 1.05f;
    int         max_new_tokens = 0;  // 0 = engine default
    std::string default_script;

    // Load `path`, applying values on top of the defaults above.
    // Throws std::runtime_error only when the file exists but cannot be read.
    static Config load(const std::string& path);

    // Fill gguf paths from models_root when they were not set explicitly.
    void derive_paths();

    // Human readable dump used by --print-config and the startup log.
    std::string to_string() const;
};

std::string trim(const std::string& s);
bool        path_exists(const std::string& p);
std::string join_path(const std::string& a, const std::string& b);

// -----------------------------------------------------------------------------
//  Wav - minimal, dependency free PCM container
// -----------------------------------------------------------------------------
//  We only need two operations: decode an arbitrary reference clip uploaded by
//  a client, and emit 24 kHz mono WAV for the legacy /tts endpoint. Anything
//  exotic (ADPCM, mp3 inside RIFF, ...) is rejected rather than guessed.
struct AudioBuffer {
    std::vector<float> samples;  // mono, [-1, 1]
    int                sample_rate = 0;

    size_t size() const { return samples.size(); }
};

// Decode a RIFF/WAVE file. Supports PCM 8/16/24/32-bit and IEEE float 32/64.
// Throws std::runtime_error with a client-safe message on malformed input.
AudioBuffer wav_decode(const uint8_t* data, size_t size);

// Encode 16-bit PCM WAV (this is what /tts returns).
std::vector<uint8_t> wav_encode(const AudioBuffer& audio);

// Resample to `target_rate` with linear interpolation.
// Reference clips only feed a speaker encoder, so linear interpolation is
// adequate; it is documented here so nobody reuses it for playback audio.
AudioBuffer resample_linear(const AudioBuffer& in, int target_rate);

// -----------------------------------------------------------------------------
//  WSOLA - time stretch without pitch shift
// -----------------------------------------------------------------------------
//  The legacy server applied WSOLA so the game could ask for a speaking rate
//  (rate > 1 = faster) without changing pitch. The algorithm is the classic
//  waveform-similarity overlap-add: fixed analysis hop, overlap search of
//  +-search_ms around the nominal synthesis hop, cross-faded Hann windows.
namespace wsola {
// Tuning knobs. All zero = pick the defaults for the direction of the stretch
// (slowing down needs a longer window and a wider pitch search: see wsola.cpp).
struct Params {
    int window_ms = 0;  // analysis/synthesis window; 0 = auto
    int search_ms = 0;  // +- search range for the similarity match; 0 = window/2
    int step      = 0;  // search step in samples; 0 = 1 (slow down) / 2 (speed up)
};

// rate: 1.0 = unchanged, 2.0 = twice as fast (half the samples).
AudioBuffer stretch(const AudioBuffer& in, float rate);
AudioBuffer stretch(const AudioBuffer& in, float rate, const Params& params);
}  // namespace wsola

// -----------------------------------------------------------------------------
//  Perf - lock-free-ish ring buffer of request timings
// -----------------------------------------------------------------------------
//  Mirrors the legacy /perf payload: rolling p50/p95/max per stage so a
//  benchmark script can read numbers without parsing logs.
class Perf {
public:
    struct Sample {
        std::string kind;       // "tts" | "pcm" | "design" | "clone"
        double      total_ms = 0;
        double      first_ms = 0;   // time to first audio (streaming only)
        double      audio_sec = 0;
        size_t      text_len = 0;
        float       rate = 1.0f;
        bool        ok = true;
    };

    void record(const Sample& s);
    std::string json_summary() const;  // ready to be sent by GET /perf
    void clear();

private:
    mutable std::mutex  mu_;
    std::vector<Sample> ring_;
    size_t              next_ = 0;
    static constexpr size_t kCapacity = 128;
};

// -----------------------------------------------------------------------------
//  VoiceStore - the voice registry
// -----------------------------------------------------------------------------
//  Layout (identical to the legacy server, plus a binary latent cache):
//    data_dir/voices.json                  metadata, id keyed
//    data_dir/voices_cache/<id>/ref.wav    reference audio (24 kHz mono)
//    data_dir/voices_cache/<id>/ref.spk    speaker embedding (float32)
//    data_dir/voices_cache/<id>/ref.rvq    RVQ codes (int32, [K][T])
//
//  The .spk/.rvq files are an optimisation: encoding a reference clip costs a
//  speaker-encoder + codec pass (~0.2-1 s) that would otherwise run on every
//  server start or every first request for that voice.
struct Voice {
    std::string id;
    std::string type;         // "designed" | "cloned"
    std::string description;
    std::string script;       // text spoken in the design reference
    // Transcript of the reference clip, kept separately because the legacy
    // voices.json distinguishes the two: designed voices use `script` (what
    // the design model was told to say), cloned voices use `ref_text` (what
    // the uploaded clip actually says). ICL conditioning needs the latter.
    std::string ref_text;
    std::string language;
    std::string created;
    bool        x_vector_only = false;
};

struct VoiceRef {             // engine-side conditioning, owned buffers
    std::vector<float>   spk;
    std::vector<int32_t> codes;
    int                  codebooks = 0;
    int                  frames = 0;
    bool                 has_codes() const { return frames > 0 && codebooks > 0; }
};

class VoiceStore {
public:
    explicit VoiceStore(std::string data_dir);

    void load();                                   // read voices.json
    std::vector<Voice> list() const;
    bool  get(const std::string& id, Voice* out) const;
    bool  exists(const std::string& id) const;
    bool  erase(const std::string& id);            // false when absent

    // Register a designed voice: `ref` is the audio the VoiceDesign model
    // produced from `description`; it becomes the clone reference.
    Voice add_designed(const std::string& id, const std::string& description,
                       const std::string& script, const std::string& language,
                       const AudioBuffer& ref);
    // Register a cloned voice from a user supplied reference clip.
    Voice add_cloned(const std::string& id, const AudioBuffer& ref,
                     const std::string& ref_text, const std::string& language);

    // Reference audio path for an existing voice ("" when missing).
    std::string ref_path(const std::string& id) const;

    // Latent cache round-trip.
    bool load_latents(const std::string& id, VoiceRef* out) const;
    void save_latents(const std::string& id, const VoiceRef& ref) const;

    static bool valid_id(const std::string& id);

private:
    void save() const;
    std::string dir_for(const std::string& id) const;

    std::string               data_dir_;
    std::map<std::string, Voice> voices_;
    mutable std::mutex        mu_;
};

// -----------------------------------------------------------------------------
//  Engine - wrapper around the vendored C ABI
// -----------------------------------------------------------------------------
//  Two contexts are needed because Qwen3-TTS ships two checkpoints:
//    * VoiceDesign : text + style instruction -> speech (used to mint voices)
//    * Base        : speech + reference latents -> speech (used to speak)
//  Both are loaded lazily and share a single codec GGUF.
class Engine {
public:
    struct Request {
        std::string text;
        std::string language;     // "Chinese", "English", ... or empty for auto
        std::string instruct;     // style instruction (base model ignores it)
        float       rate = 1.0f;  // applied by WSOLA, not by the model
        // Optional sampling overrides; <= 0 / NaN means "use config default".
        float temperature = 0;
        int   top_k = 0;
        float top_p = 0;
        float repetition_penalty = 0;
    };

    Engine(Config cfg, VoiceStore* voices);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Load both talkers up front (server start). Throws on failure so the
    // process exits with a clear message instead of serving 500s.
    void preload();

    bool base_loaded() const;
    bool design_loaded() const;

    // Pin the design talker across calls. Minting a batch of voices through
    // Engine::design() otherwise reloads the 1.7B design model every time.
    void set_keep_design_loaded(bool keep);

    // Full-utterance synthesis. `out` receives 24 kHz mono float PCM.
    void synthesize(const Voice& voice, const Request& req, AudioBuffer* out);

    // Streaming synthesis: `on_chunk` is called from the engine worker thread
    // with 24 kHz mono float PCM. Returning false cancels the synthesis.
    // Only valid when stream_usable(req) is true; otherwise it throws.
    void synthesize_stream(const Voice& voice, const Request& req,
                           const std::function<bool(const float*, int)>& on_chunk,
                           int* out_sample_rate);

    // Streaming needs the waveform as a whole because WSOLA (rate control)
    // cannot splice a partial buffer. The engine *can* run a rate-changed
    // stream (buffer the utterance, stretch it, replay in ~170 ms blocks —
    // measured 864 ms to first chunk on a 30-character sentence), but the
    // default policy refuses it, for two reasons that are ours, not the
    // engine's limits:
    //   1. MOD debugging: a rate-changed line must always take the buffered
    //      /tts path, so a bug is never "sometimes stream, sometimes not".
    //   2. Wire compatibility: the legacy Python server answered 422 here and
    //      the game-side client already falls back to /tts within 30 s;
    //      refusing keeps one behaviour across both servers while they are
    //      A/B'd.
    // Set `stream_stretch = 1` in the ini to enable the capability instead.
    bool stream_usable(const Request& req) const;

    // Mint a new voice: returns the reference audio produced by VoiceDesign.
    AudioBuffer design(const std::string& description, const std::string& script,
                       const std::string& language);

    // Forget the cached conditioning of `id`. Called after DELETE /voices/{id}
    // so a re-created voice with the same id cannot inherit stale latents.
    void drop_voice(const std::string& id);

    std::string describe() const;   // one line for /health
    int         inflight() const;

private:
    struct Impl;
    Impl* impl_;
};

// -----------------------------------------------------------------------------
//  HTTP layer
// -----------------------------------------------------------------------------
struct ServerContext {
    Config      cfg;
    VoiceStore* voices = nullptr;
    Engine*     engine = nullptr;
    Perf*       perf = nullptr;
    std::string started_at;   // readable timestamp, for /health

    // Optional hooks for an embedding host (the GUI). Both are called from the
    // thread that runs run_http_server / from any thread respectively.
    std::function<void()> on_listening;   // fired once the port is bound
    std::function<void()> request_stop;   // set by run_http_server, callable from another thread
};

// Blocking: runs the HTTP server until stop() is requested or request_stop() runs.
void run_http_server(ServerContext& ctx);

// Filled by Engine / VoiceStore and used by the HTTP handlers.
std::string now_string();

}  // namespace ttsc
