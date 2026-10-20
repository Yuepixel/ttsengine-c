// =============================================================================
//  config.cpp - INI parsing, key compatible with the legacy ttsserver.ini
// =============================================================================
#include "ttsc.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ttsc {

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const auto  b  = s.find_first_not_of(ws);
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

bool path_exists(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return f.good();
}

std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const char last = a.back();
    if (last == '/' || last == '\\') return a + b;
    const bool windows = a.find('\\') != std::string::npos || (a.size() > 1 && a[1] == ':');
    return a + (windows ? "\\" : "/") + b;
}

namespace {

bool parse_bool(const std::string& v) {
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

int parse_int(const std::string& v, int fallback) {
    try { return std::stoi(v); } catch (...) { return fallback; }
}

float parse_float(const std::string& v, float fallback) {
    try { return std::stof(v); } catch (...) { return fallback; }
}

}  // namespace

Config Config::load(const std::string& path) {
    Config cfg;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        // A missing file is not an error: the defaults are usable and the
        // caller decides whether that is acceptable. Derive the gguf paths so
        // the defaults are self-consistent instead of empty.
        cfg.derive_paths();
        return cfg;
    }

    std::string line;
    bool        first = true;
    int         line_no = 0;
    while (std::getline(file, line)) {
        ++line_no;
        if (first) {
            first = false;
            // Strip a UTF-8 BOM: Notepad adds one on Windows and it would
            // otherwise become part of the first key.
            if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
        }
        const auto hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));
        if (key.empty()) continue;

        if (key == "models_root")           cfg.models_root = val;
        else if (key == "base_gguf")        cfg.base_gguf = val;
        else if (key == "design_gguf")      cfg.design_gguf = val;
        else if (key == "codec_gguf")       cfg.codec_gguf = val;
        else if (key == "data_dir")         cfg.data_dir = val;
        else if (key == "host")             cfg.host = val;
        else if (key == "port")             cfg.port = parse_int(val, cfg.port);
        else if (key == "device")           cfg.device = val;
        else if (key == "max_batch")        cfg.max_batch = parse_int(val, cfg.max_batch);
        else if (key == "keep_design_loaded") cfg.keep_design_loaded = parse_bool(val);
        else if (key == "use_fa")           cfg.use_fa = parse_bool(val);
        else if (key == "stream")           cfg.stream = parse_bool(val);
        else if (key == "stream_chunk")     cfg.stream_chunk = parse_int(val, cfg.stream_chunk);
        else if (key == "stream_stretch")   cfg.stream_stretch = parse_bool(val);
        else if (key == "time_stretch")     cfg.time_stretch = parse_bool(val);
        else if (key == "perf")             cfg.perf = parse_bool(val);
        else if (key == "debug")            cfg.debug = parse_bool(val);
        else if (key == "max_queue")        cfg.max_queue = parse_int(val, cfg.max_queue);
        else if (key == "synth_timeout")    cfg.synth_timeout_ms = parse_int(val, cfg.synth_timeout_ms) * 1000;
        else if (key == "language")         cfg.language = val;
        else if (key == "temperature")      cfg.temperature = parse_float(val, cfg.temperature);
        else if (key == "top_k")            cfg.top_k = parse_int(val, cfg.top_k);
        else if (key == "top_p")            cfg.top_p = parse_float(val, cfg.top_p);
        else if (key == "repetition_penalty") cfg.repetition_penalty = parse_float(val, cfg.repetition_penalty);
        else if (key == "max_new_tokens")   cfg.max_new_tokens = parse_int(val, cfg.max_new_tokens);
        else if (key == "default_script")   cfg.default_script = val;
        // Unknown keys are ignored on purpose: the same file is shared with
        // the legacy Python server during migration.
    }
    cfg.derive_paths();
    return cfg;
}

void Config::derive_paths() {
    if (models_root.empty()) models_root = "models";

    const auto pick = [this](const std::string& name) {
        return join_path(models_root, name);
    };
    if (base_gguf.empty())   base_gguf = pick("qwen-talker-1.7b-base-Q8_0.gguf");
    if (design_gguf.empty()) design_gguf = pick("qwen-talker-1.7b-voicedesign-Q8_0.gguf");
    if (codec_gguf.empty())  codec_gguf = pick("qwen-tokenizer-12hz-Q8_0.gguf");
    if (data_dir.empty())    data_dir = "data";
}

std::string Config::to_string() const {
    std::ostringstream os;
    os << "config:\n"
       << "  models_root       = " << models_root << "\n"
       << "  base_gguf         = " << base_gguf << "\n"
       << "  design_gguf       = " << design_gguf << "\n"
       << "  codec_gguf        = " << codec_gguf << "\n"
       << "  data_dir          = " << data_dir << "\n"
       << "  host:port         = " << host << ":" << port << "\n"
       << "  device            = " << (device.empty() ? "(auto)" : device) << "\n"
       << "  max_batch         = " << max_batch << "\n"
       << "  stream            = " << (stream ? 1 : 0) << "\n"
       << "  stream_stretch    = " << (stream_stretch ? 1 : 0) << "\n"
       << "  time_stretch      = " << (time_stretch ? 1 : 0) << "\n"
       << "  keep_design_loaded= " << (keep_design_loaded ? 1 : 0) << "\n"
       << "  max_queue         = " << max_queue << "\n"
       << "  sampling          = T " << temperature << " / top_k " << top_k
       << " / top_p " << top_p << " / rep " << repetition_penalty << "\n";
    return os.str();
}

std::string now_string() {
    // Local time in the same format as the legacy server: "2026-10-20 18:04:11".
    std::time_t t = std::time(nullptr);
    std::tm     tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

}  // namespace ttsc
