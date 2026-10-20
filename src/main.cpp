// =============================================================================
//  main.cpp - ttsc-server entry point
//
//  Two modes:
//    * serve (default)  - load the engine, warm it up, run the HTTP surface.
//    * one shot         - --say "<text>" or --text-file <path> [-o out.wav]
//                         synthesises once and exits. This is what the
//                         benchmark script and the smoke tests drive; it also
//                         keeps the engine testable before stage 3 lands.
//
//  Everything else (--print-config, --list-voices, --help) is diagnostic.
// =============================================================================
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

#include "ttsc.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

const char* kUsage =
    "ttsc-server - Qwen3-TTS service (C++/GGML)\n"
    "\n"
    "usage: ttsc-server [options]\n"
    "\n"
    "  -c, --config <path>   ini file (default: ttsserver.ini next to the exe)\n"
    "      --serve           run the HTTP server (default)\n"
    "      --say <text>      synthesise one utterance and exit\n"
    "      --text-file <p>   synthesise the UTF-8 text in <p> and exit\n"
    "  -o, --out <p>         WAV output for one-shot mode (default: out.wav)\n"
    "      --voice <id>      voice id (default: first registered voice)\n"
    "      --rate <f>        0.5-2.0 speaking rate via WSOLA (default 1.0)\n"
    "      --lang <name>     language hint, e.g. Chinese / English\n"
    "      --stream          one-shot via the streaming callback path\n"
    "                        (rate != 1 is refused unless ini stream_stretch=1)\n"
    "      --stretch <wav>   time-stretch an existing WAV by --rate and exit\n"
    "                        (diagnostic: isolates WSOLA from the model)\n"
    "      --win-ms <n>      WSOLA window override (default: 64 slow / 30 fast)\n"
    "      --search-ms <n>   WSOLA +- search override (default: win/2 slow / 10 fast)\n"
    "      --search-step <n> WSOLA search step in samples (default: 1 slow / 2 fast)\n"
    "      --no-warmup       skip the start-up warm-up synthesis\n"
    "      --print-config    dump the resolved configuration and exit\n"
    "      --list-voices     list registered voices and exit\n"
    "  -h, --help            this text\n";

struct Args {
    std::string config = "ttsserver.ini";
    std::string out    = "out.wav";
    std::string voice;
    std::string say;
    std::string text_file;
    std::string stretch;
    std::string lang;
    float       rate      = 1.0f;
    ttsc::wsola::Params wsola_params;
    bool        serve     = false;
    bool        print_cfg = false;
    bool        list      = false;
    bool        warmup    = true;
    bool        stream    = false;
    bool        help      = false;
};

std::string read_text_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + p);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    // Strip a BOM: the file is edited on Windows and Notepad still adds one.
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
    return s;
}

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + what);
            return argv[++i];
        };
        if (k == "-c" || k == "--config")      a.config    = next("--config");
        else if (k == "--say")                 a.say       = next("--say");
        else if (k == "--text-file")           a.text_file = next("--text-file");
        else if (k == "--stretch")             a.stretch   = next("--stretch");
        else if (k == "-o" || k == "--out")    a.out       = next("--out");
        else if (k == "--voice")               a.voice     = next("--voice");
        else if (k == "--lang")                a.lang      = next("--lang");
        else if (k == "--rate")                a.rate      = std::stof(next("--rate"));
        else if (k == "--win-ms")              a.wsola_params.window_ms = std::stoi(next("--win-ms"));
        else if (k == "--search-ms")           a.wsola_params.search_ms = std::stoi(next("--search-ms"));
        else if (k == "--search-step")         a.wsola_params.step      = std::stoi(next("--search-step"));
        else if (k == "--serve")               a.serve     = true;
        else if (k == "--stream")              a.stream    = true;
        else if (k == "--no-warmup")           a.warmup    = false;
        else if (k == "--print-config")        a.print_cfg = true;
        else if (k == "--list-voices")         a.list      = true;
        else if (k == "-h" || k == "--help")   a.help      = true;
        else throw std::runtime_error("unknown option: " + k);
    }
    if (!a.say.empty() && !a.text_file.empty()) {
        throw std::runtime_error("--say and --text-file are mutually exclusive");
    }
    // No explicit action and no diagnostic -> serve.
    if (!a.serve && a.say.empty() && a.text_file.empty() && !a.print_cfg && !a.list && !a.help) {
        a.serve = true;
    }
    return a;
}

bool write_file(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    return f.good();
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    // Chinese text on stdin/stdout/argv: the console defaults to the ANSI code
    // page, which turns UTF-8 into mojibake.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    try {
        const Args args = parse_args(argc, argv);
        if (args.help) {
            std::fputs(kUsage, stdout);
            return 0;
        }

        ttsc::Config cfg = ttsc::Config::load(args.config);
        if (args.print_cfg) {
            std::fputs(cfg.to_string().c_str(), stdout);
            return 0;
        }

        if (!args.stretch.empty()) {
            // Offline stretch: lets us judge WSOLA on a fixed waveform instead
            // of comparing two independently sampled syntheses.
            std::ifstream f(args.stretch, std::ios::binary);
            if (!f) throw std::runtime_error("cannot open " + args.stretch);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                       std::istreambuf_iterator<char>());
            ttsc::AudioBuffer in = ttsc::wav_decode(bytes.data(), bytes.size());
            ttsc::AudioBuffer out = ttsc::wsola::stretch(in, args.rate, args.wsola_params);
            const auto wav = ttsc::wav_encode(out);
            if (!write_file(args.out, wav)) throw std::runtime_error("cannot write " + args.out);
            std::printf("stretched %s -> %s: %.2f s -> %.2f s (rate %.2f)\n",
                        args.stretch.c_str(), args.out.c_str(),
                        double(in.samples.size()) / double(in.sample_rate),
                        double(out.samples.size()) / double(out.sample_rate), args.rate);
            return 0;
        }

        ttsc::VoiceStore voices(cfg.data_dir);
        voices.load();

        if (args.list) {
            const auto all = voices.list();
            std::printf("%zu voice(s) in %s\n", all.size(), cfg.data_dir.c_str());
            for (const auto& v : all) {
                std::printf("  %-24s %-8s %s\n", v.id.c_str(), v.type.c_str(),
                            v.description.c_str());
            }
            return 0;
        }

        const bool one_shot = !args.say.empty() || !args.text_file.empty();

        ttsc::Engine engine(cfg, &voices);
        if (args.warmup || one_shot) engine.preload();
        else std::fprintf(stderr, "[ttsc] warm-up skipped\n");

        if (one_shot) {
            std::string text = args.say.empty() ? read_text_file(args.text_file) : args.say;
            if (ttsc::trim(text).empty()) throw std::runtime_error("input text is empty");

            ttsc::Voice voice;
            if (!args.voice.empty()) {
                if (!voices.get(args.voice, &voice)) {
                    throw std::runtime_error("unknown voice: " + args.voice);
                }
            } else {
                const auto all = voices.list();
                if (all.empty()) throw std::runtime_error("no voices registered in " + cfg.data_dir);
                voice = all.front();
            }

            ttsc::Engine::Request req;
            req.text     = text;
            req.language = args.lang.empty() ? voice.language : args.lang;
            req.rate     = args.rate;

            ttsc::AudioBuffer audio;
            if (args.stream) {
                // Exercise the callback path end to end: chunks arrive on the
                // engine's compute thread, so the sink only appends.
                int         chunks = 0;
                double      first_ms = 0;
                const auto  t0 = std::chrono::steady_clock::now();
                int         rate = 0;
                engine.synthesize_stream(
                    voice, req,
                    [&](const float* samples, int n) {
                        if (chunks == 0) {
                            first_ms = std::chrono::duration<double, std::milli>(
                                           std::chrono::steady_clock::now() - t0)
                                           .count();
                        }
                        audio.samples.insert(audio.samples.end(), samples, samples + n);
                        ++chunks;
                        return true;
                    },
                    &rate);
                audio.sample_rate = rate > 0 ? rate : 24000;
                std::fprintf(stderr, "[ttsc] stream: %d chunk(s), first chunk at %.0f ms\n",
                             chunks, first_ms);
            } else {
                engine.synthesize(voice, req, &audio);
            }
            if (audio.samples.empty()) throw std::runtime_error("engine returned no audio");

            const auto wav = ttsc::wav_encode(audio);
            if (!write_file(args.out, wav)) throw std::runtime_error("cannot write " + args.out);

            const double secs = double(audio.samples.size()) / double(audio.sample_rate);
            std::printf("wrote %s: %.2f s audio, %d Hz, voice=%s rate=%.2f\n", args.out.c_str(),
                        secs, audio.sample_rate, voice.id.c_str(), args.rate);
            return 0;
        }

        ttsc::Perf       perf;
        ttsc::ServerContext ctx;
        ctx.cfg        = cfg;
        ctx.voices     = &voices;
        ctx.engine     = &engine;
        ctx.perf       = &perf;
        ctx.started_at = ttsc::now_string();

        std::fprintf(stderr, "[ttsc] %s\n", engine.describe().c_str());
        ttsc::run_http_server(ctx);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[ttsc] error: %s\n", e.what());
        return 1;
    }
}
