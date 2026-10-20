// =============================================================================
//  voice_store.cpp - voice registry (metadata + cached latents)
//
//  File format compatibility
//    voices.json is deliberately the same shape the Python server produced:
//      { "<id>": { "id":..., "type":"designed"|"cloned", "description":...,
//                  "script":..., "language":..., "ref_audio":..., "created":... } }
//    so an existing deployment keeps its voices when switching binaries.
//    Two extra files per voice (ref.spk / ref.rvq) cache the encoded
//    conditioning; they are ignored by the legacy server and regenerated on
//    demand when missing.
// =============================================================================
#include "ttsc.h"

#include "yyjson.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

#if defined(_WIN32)
#include <direct.h>
#define TTSC_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define TTSC_MKDIR(p) mkdir(p, 0755)
#endif

namespace ttsc {
namespace {

void ensure_dir(const std::string& p) {
    if (p.empty()) return;
    std::string cur;
    for (size_t i = 0; i < p.size(); ++i) {
        cur.push_back(p[i]);
        const bool sep = p[i] == '/' || p[i] == '\\';
        if (sep && cur.size() > 1 && cur[cur.size() - 2] != ':') {
            if (!path_exists(cur)) TTSC_MKDIR(cur.c_str());
        }
    }
    if (!path_exists(p)) TTSC_MKDIR(p.c_str());
}

Voice voice_from_json(yyjson_val* obj) {
    Voice v;
    auto str = [&](const char* k) -> std::string {
        yyjson_val* x = yyjson_obj_get(obj, k);
        return (x && yyjson_is_str(x)) ? yyjson_get_str(x) : "";
    };
    v.id          = str("id");
    v.type        = str("type");
    v.description = str("description");
    v.script      = str("script");
    v.ref_text    = str("ref_text");
    v.language    = str("language");
    v.created     = str("created");
    yyjson_val* xv = yyjson_obj_get(obj, "x_vector_only");
    v.x_vector_only = xv && yyjson_is_bool(xv) && yyjson_get_bool(xv);
    // Files written before ref_text existed only carried `script`; treat that
    // as the transcript so old designed voices keep using ICL mode B.
    if (v.ref_text.empty()) v.ref_text = v.script;
    return v;
}

std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    o += buf;
                } else {
                    o.push_back(c);
                }
        }
    }
    return o;
}

}  // namespace

VoiceStore::VoiceStore(std::string data_dir) : data_dir_(std::move(data_dir)) {}

bool VoiceStore::valid_id(const std::string& id) {
    // Same rule as the Python server: 1-64 chars, starts alphanumeric, no
    // separators - this is the only guard between a voice id and a path.
    if (id.empty() || id.size() > 64) return false;
    const auto ok_first = [](unsigned char c) { return std::isalnum(c) != 0; };
    if (!ok_first(static_cast<unsigned char>(id[0]))) return false;
    for (char c : id) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.'))
            return false;
    }
    return id != "." && id != "..";
}

std::string VoiceStore::dir_for(const std::string& id) const {
    return join_path(join_path(data_dir_, "voices_cache"), id);
}

std::string VoiceStore::ref_path(const std::string& id) const {
    return join_path(dir_for(id), "ref.wav");
}

void VoiceStore::load() {
    ensure_dir(data_dir_);
    const std::string path = join_path(data_dir_, "voices.json");
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (text.empty()) return;

    yyjson_doc* doc = yyjson_read(text.c_str(), text.size(), 0);
    if (!doc) throw std::runtime_error("voices.json: parse error");
    yyjson_val* root = yyjson_doc_get_root(doc);
    if (yyjson_is_obj(root)) {
        yyjson_obj_iter it = yyjson_obj_iter_with(root);
        yyjson_val*    key;
        while ((key = yyjson_obj_iter_next(&it))) {
            yyjson_val* val = yyjson_obj_iter_get_val(key);
            if (!yyjson_is_obj(val)) continue;
            Voice v = voice_from_json(val);
            if (v.id.empty()) v.id = yyjson_get_str(key);
            if (!valid_id(v.id)) continue;
            voices_[v.id] = v;
        }
    }
    yyjson_doc_free(doc);
}

void VoiceStore::save() const {
    ensure_dir(data_dir_);
    const std::string path = join_path(data_dir_, "voices.json");
    std::string out = "{\n";
    bool first = true;
    for (const auto& kv : voices_) {
        const Voice& v = kv.second;
        if (!first) out += ",\n";
        first = false;
        out += "  \"" + json_escape(v.id) + "\": {";
        out += "\"id\":\"" + json_escape(v.id) + "\",";
        out += "\"type\":\"" + json_escape(v.type) + "\",";
        out += "\"description\":\"" + json_escape(v.description) + "\",";
        out += "\"script\":\"" + json_escape(v.script) + "\",";
        out += "\"language\":\"" + json_escape(v.language) + "\",";
        out += "\"ref_audio\":\"" + json_escape(ref_path(v.id)) + "\",";
        out += "\"ref_text\":\"" + json_escape(v.ref_text) + "\",";
        out += "\"x_vector_only\":" + std::string(v.x_vector_only ? "true" : "false") + ",";
        out += "\"created\":\"" + json_escape(v.created) + "\"";
        out += "}";
    }
    out += "\n}\n";
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << out;
}

std::vector<Voice> VoiceStore::list() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Voice> out;
    out.reserve(voices_.size());
    for (const auto& kv : voices_) out.push_back(kv.second);
    return out;
}

bool VoiceStore::get(const std::string& id, Voice* out) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = voices_.find(id);
    if (it == voices_.end()) return false;
    if (out) *out = it->second;
    return true;
}

bool VoiceStore::exists(const std::string& id) const { return get(id, nullptr); }

bool VoiceStore::erase(const std::string& id) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (voices_.erase(id) == 0) return false;
        save();
    }
    // Remove the cached files. std::filesystem is avoided on purpose: the
    // project builds with C++17 but without <filesystem> on older MSVC STLs.
    const std::string dir = dir_for(id);
    std::remove(join_path(dir, "ref.wav").c_str());
    std::remove(join_path(dir, "ref.spk").c_str());
    std::remove(join_path(dir, "ref.rvq").c_str());
#if defined(_WIN32)
    _rmdir(dir.c_str());
#else
    rmdir(dir.c_str());
#endif
    return true;
}

Voice VoiceStore::add_designed(const std::string& id, const std::string& description,
                               const std::string& script, const std::string& language,
                               const AudioBuffer& ref) {
    if (!valid_id(id)) throw std::runtime_error("illegal voice id");
    ensure_dir(dir_for(id));
    const auto wav = wav_encode(ref);
    std::ofstream f(ref_path(id), std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(wav.data()), std::streamsize(wav.size()));
    f.close();

    Voice v;
    v.id          = id;
    v.type        = "designed";
    v.description = description;
    v.script      = script;
    v.ref_text    = script;   // the design model was told to speak exactly this
    v.language    = language;
    v.created     = now_string();
    {
        std::lock_guard<std::mutex> lk(mu_);
        voices_[id] = v;
        save();
    }
    return v;
}

Voice VoiceStore::add_cloned(const std::string& id, const AudioBuffer& ref,
                             const std::string& ref_text, const std::string& language) {
    if (!valid_id(id)) throw std::runtime_error("illegal voice id");
    ensure_dir(dir_for(id));
    const auto wav = wav_encode(ref);
    std::ofstream f(ref_path(id), std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(wav.data()), std::streamsize(wav.size()));
    f.close();

    Voice v;
    v.id          = id;
    v.type        = "cloned";
    v.description = ref_text;
    v.ref_text    = ref_text;
    // Without a transcript the reference can only anchor the speaker vector
    // (clone mode A); with one the engine also replays the RVQ codes (mode B).
    v.x_vector_only = trim(ref_text).empty();
    v.language    = language;
    v.created     = now_string();
    {
        std::lock_guard<std::mutex> lk(mu_);
        voices_[id] = v;
        save();
    }
    return v;
}

// --- latent cache -----------------------------------------------------------
//  Binary layouts, both little endian:
//    ref.spk : "TSPK" u32 version=1 u32 dim float[dim]
//    ref.rvq : "TRVQ" u32 version=1 u32 codebooks u32 frames i32[codebooks*frames]
bool VoiceStore::load_latents(const std::string& id, VoiceRef* out) const {
    if (!out) return false;
    const std::string dir = dir_for(id);
    std::ifstream spk(join_path(dir, "ref.spk"), std::ios::binary);
    // ref.rvq is optional: an x_vector_only voice carries the speaker
    // embedding alone and never had a code matrix encoded.
    std::ifstream rvq(join_path(dir, "ref.rvq"), std::ios::binary);
    if (!spk) return false;

    char magic[4];
    uint32_t ver = 0, dim = 0;
    spk.read(magic, 4);
    spk.read(reinterpret_cast<char*>(&ver), 4);
    spk.read(reinterpret_cast<char*>(&dim), 4);
    if (std::memcmp(magic, "TSPK", 4) != 0 || ver != 1 || dim == 0 || dim > 1 << 16) return false;
    out->spk.resize(dim);
    spk.read(reinterpret_cast<char*>(out->spk.data()), std::streamsize(dim * sizeof(float)));
    if (!spk) return false;

    if (!rvq) return true;   // speaker-only reference

    uint32_t k = 0, t = 0;
    rvq.read(magic, 4);
    rvq.read(reinterpret_cast<char*>(&ver), 4);
    rvq.read(reinterpret_cast<char*>(&k), 4);
    rvq.read(reinterpret_cast<char*>(&t), 4);
    if (std::memcmp(magic, "TRVQ", 4) != 0 || ver != 1 || k == 0 || t == 0) {
        // A corrupt code cache must not invalidate the embedding we already
        // decoded; the caller simply falls back to clone mode A.
        return true;
    }
    out->codes.resize(size_t(k) * t);
    rvq.read(reinterpret_cast<char*>(out->codes.data()),
             std::streamsize(out->codes.size() * sizeof(int32_t)));
    if (!rvq) {
        out->codes.clear();
        return true;
    }
    out->codebooks = int(k);
    out->frames    = int(t);
    return true;
}

void VoiceStore::save_latents(const std::string& id, const VoiceRef& ref) const {
    const std::string dir = dir_for(id);
    ensure_dir(dir);
    {
        std::ofstream spk(join_path(dir, "ref.spk"), std::ios::binary | std::ios::trunc);
        const uint32_t ver = 1, dim = uint32_t(ref.spk.size());
        spk.write("TSPK", 4);
        spk.write(reinterpret_cast<const char*>(&ver), 4);
        spk.write(reinterpret_cast<const char*>(&dim), 4);
        spk.write(reinterpret_cast<const char*>(ref.spk.data()),
                  std::streamsize(ref.spk.size() * sizeof(float)));
    }
    if (!ref.has_codes()) {
        // A speaker-only reference means the cached code matrix is stale (the
        // voice was re-saved in x_vector_only mode). Drop it, otherwise the
        // next load pairs the new embedding with the old codes.
        std::remove(join_path(dir, "ref.rvq").c_str());
        return;
    }
    std::ofstream rvq(join_path(dir, "ref.rvq"), std::ios::binary | std::ios::trunc);
    const uint32_t ver = 1, k = uint32_t(ref.codebooks), t = uint32_t(ref.frames);
    rvq.write("TRVQ", 4);
    rvq.write(reinterpret_cast<const char*>(&ver), 4);
    rvq.write(reinterpret_cast<const char*>(&k), 4);
    rvq.write(reinterpret_cast<const char*>(&t), 4);
    rvq.write(reinterpret_cast<const char*>(ref.codes.data()),
              std::streamsize(ref.codes.size() * sizeof(int32_t)));
}

}  // namespace ttsc
