// =============================================================================
//  test_config.cpp - ini parsing stays compatible with the legacy server
// =============================================================================
#include "test.h"
#include "ttsc.h"

#include <cstdio>
#include <fstream>
#include <string>

namespace {

// Temp files live next to the test binary: the build tree is writable and the
// cleanup is a plain std::remove, so no dependency on <filesystem>.
std::string write_temp(const std::string& name, const std::string& body, bool bom = false) {
    const std::string path = name;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (bom) f.put(char(0xEF)), f.put(char(0xBB)), f.put(char(0xBF));
    f << body;
    f.close();
    return path;
}

}  // namespace

TTSC_TEST(config_defaults_when_file_missing) {
    const ttsc::Config cfg = ttsc::Config::load("does-not-exist-ttsc.ini");
    CHECK_EQ(cfg.port, 8080);
    CHECK_EQ(cfg.host, std::string("127.0.0.1"));
    CHECK_EQ(cfg.max_batch, 1);
    CHECK(cfg.time_stretch);
    CHECK_EQ(cfg.language, std::string("Chinese"));
    CHECK_EQ(cfg.models_root, std::string("models"));
}

TTSC_TEST(config_reads_legacy_keys) {
    const std::string path = write_temp(
        "ttsc_test_config.ini",
        "models_root = C:\\models\\qwen3-tts-gguf\n"
        "port = 8090\n"
        "host = 0.0.0.0\n"
        "max_batch = 4\n"
        "time_stretch = 0\n"
        "synth_timeout = 30\n"
        "temperature = 0.7\n"
        "unknown_key = ignored\n"
        "# comment line\n");

    const ttsc::Config cfg = ttsc::Config::load(path);
    CHECK_EQ(cfg.models_root, std::string("C:\\models\\qwen3-tts-gguf"));
    CHECK_EQ(cfg.port, 8090);
    CHECK_EQ(cfg.host, std::string("0.0.0.0"));
    CHECK_EQ(cfg.max_batch, 4);
    CHECK(!cfg.time_stretch);
    CHECK_EQ(cfg.synth_timeout_ms, 30000);   // ini is seconds, struct is ms
    CHECK_NEAR(cfg.temperature, 0.7, 1e-6);
    // derive_paths() fills the gguf names from models_root.
    CHECK(cfg.base_gguf.find("qwen3-tts-gguf") != std::string::npos);
    CHECK(cfg.base_gguf.find("base") != std::string::npos);
    CHECK(cfg.codec_gguf.find("tokenizer") != std::string::npos);
    std::remove(path.c_str());
}

TTSC_TEST(config_tolerates_utf8_bom_and_crlf) {
    const std::string path = write_temp("ttsc_test_bom.ini", "port = 8123\r\nmax_batch = 2\r\n", true);
    const ttsc::Config cfg = ttsc::Config::load(path);
    CHECK_EQ(cfg.port, 8123);
    CHECK_EQ(cfg.max_batch, 2);
    std::remove(path.c_str());
}

TTSC_TEST(config_explicit_paths_win_over_derive) {
    const std::string path = write_temp("ttsc_test_paths.ini",
                                        "base_gguf = C:\\x\\base.gguf\n"
                                        "design_gguf = C:\\x\\design.gguf\n"
                                        "codec_gguf = C:\\x\\codec.gguf\n");
    const ttsc::Config cfg = ttsc::Config::load(path);
    CHECK_EQ(cfg.base_gguf, std::string("C:\\x\\base.gguf"));
    CHECK_EQ(cfg.design_gguf, std::string("C:\\x\\design.gguf"));
    CHECK_EQ(cfg.codec_gguf, std::string("C:\\x\\codec.gguf"));
    std::remove(path.c_str());
}

TTSC_TEST(trim_and_join_path) {
    CHECK_EQ(ttsc::trim("  a b \r\n"), std::string("a b"));
    CHECK_EQ(ttsc::trim("   "), std::string(""));
    CHECK_EQ(ttsc::join_path("C:\\models", "x.gguf"), std::string("C:\\models\\x.gguf"));
    CHECK_EQ(ttsc::join_path("C:\\models\\", "x.gguf"), std::string("C:\\models\\x.gguf"));
    CHECK_EQ(ttsc::join_path("", "x.gguf"), std::string("x.gguf"));
}
