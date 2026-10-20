// =============================================================================
//  test_voice_store.cpp - registry round trip and the .spk/.rvq latent cache
// =============================================================================
#include "test.h"
#include "ttsc.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

#if defined(_WIN32)
#include <direct.h>
#endif

namespace {

const char* kDataDir = "ttsc_test_data";

void wipe() {
    std::remove("ttsc_test_data\\voices.json");
    std::remove("ttsc_test_data\\voices_cache\\alice\\ref.wav");
    std::remove("ttsc_test_data\\voices_cache\\alice\\ref.spk");
    std::remove("ttsc_test_data\\voices_cache\\alice\\ref.rvq");
    std::remove("ttsc_test_data\\voices_cache\\bob\\ref.wav");
    std::remove("ttsc_test_data\\voices_cache\\bob\\ref.spk");
    std::remove("ttsc_test_data\\voices_cache\\bob\\ref.rvq");
#if defined(_WIN32)
    _rmdir("ttsc_test_data\\voices_cache\\alice");
    _rmdir("ttsc_test_data\\voices_cache\\bob");
    _rmdir("ttsc_test_data\\voices_cache");
    _rmdir("ttsc_test_data");
#else
    rmdir("ttsc_test_data/voices_cache/alice");
    rmdir("ttsc_test_data/voices_cache/bob");
    rmdir("ttsc_test_data/voices_cache");
    rmdir("ttsc_test_data");
#endif
}

ttsc::AudioBuffer blip(double seconds) {
    ttsc::AudioBuffer a;
    a.sample_rate = 24000;
    const size_t n = size_t(seconds * 24000);
    a.samples.resize(n);
    for (size_t i = 0; i < n; ++i) {
        a.samples[i] = 0.3f * std::sin(2.0f * 3.14159265f * 180.0f * float(i) / 24000.0f);
    }
    return a;
}

}  // namespace

TTSC_TEST(voice_store_add_and_reload) {
    wipe();
    {
        ttsc::VoiceStore store(kDataDir);
        store.load();
        CHECK(store.list().empty());
        CHECK_THROWS(store.add_cloned("bad id", blip(0.1), "", "Chinese"));

        const ttsc::Voice v = store.add_cloned("alice", blip(0.1), "hello", "Chinese");
        CHECK_EQ(v.id, std::string("alice"));
        CHECK_EQ(v.type, std::string("cloned"));
        CHECK(store.exists("alice"));
        CHECK_EQ(store.list().size(), size_t(1));
        CHECK(store.ref_path("alice").find("ref.wav") != std::string::npos);
    }
    // A second store instance must see the persisted metadata and audio.
    ttsc::VoiceStore again(kDataDir);
    again.load();
    CHECK(again.exists("alice"));
    ttsc::Voice out;
    CHECK(again.get("alice", &out));
    CHECK_EQ(out.language, std::string("Chinese"));
    CHECK(ttsc::path_exists(again.ref_path("alice")));

    CHECK(again.erase("alice"));
    CHECK(!again.exists("alice"));
    CHECK(!again.erase("alice"));
    wipe();
}

TTSC_TEST(voice_store_latent_roundtrip) {
    wipe();
    ttsc::VoiceStore store(kDataDir);
    store.load();
    store.add_cloned("bob", blip(0.1), "hi", "English");

    ttsc::VoiceRef ref;
    CHECK(!store.load_latents("bob", &ref));   // nothing cached yet

    ref.spk = {0.25f, -0.5f, 1.5f};
    ref.codes = {1, 2, 3, 4, 5, 6};
    ref.codebooks = 2;
    ref.frames = 3;
    store.save_latents("bob", ref);

    ttsc::VoiceRef back;
    CHECK(store.load_latents("bob", &back));
    CHECK_EQ(back.spk.size(), size_t(3));
    CHECK_NEAR(back.spk[2], 1.5, 1e-6);
    CHECK_EQ(back.codebooks, 2);
    CHECK_EQ(back.frames, 3);
    CHECK_EQ(back.codes.size(), size_t(6));
    CHECK_EQ(back.codes[5], 6);
    CHECK(back.has_codes());

    // Speaker-only reference: no .rvq is written, has_codes() stays false.
    ttsc::VoiceRef spk_only;
    spk_only.spk = {1.0f, 2.0f};
    store.save_latents("bob", spk_only);
    ttsc::VoiceRef back2;
    CHECK(store.load_latents("bob", &back2));
    CHECK_EQ(back2.spk.size(), size_t(2));
    CHECK(!back2.has_codes());
    wipe();
}

TTSC_TEST(voice_store_rejects_corrupt_latents) {
    wipe();
    ttsc::VoiceStore store(kDataDir);
    store.load();
    store.add_cloned("alice", blip(0.1), "x", "Chinese");
    ttsc::VoiceRef ref;
    ref.spk = {0.1f};
    store.save_latents("alice", ref);

    // Corrupt the magic: the loader must fail instead of trusting the file.
    std::fstream f("ttsc_test_data\\voices_cache\\alice\\ref.spk",
                   std::ios::binary | std::ios::in | std::ios::out);
    CHECK(f.good());
    f.seekp(0);
    f.write("XXXX", 4);
    f.close();

    ttsc::VoiceRef out;
    CHECK(!store.load_latents("alice", &out));
    wipe();
}

TTSC_TEST(voice_store_valid_id_rules) {
    CHECK(ttsc::VoiceStore::valid_id("vega"));
    CHECK(ttsc::VoiceStore::valid_id("a_b-c.d"));
    CHECK(ttsc::VoiceStore::valid_id("1abc"));
    CHECK(!ttsc::VoiceStore::valid_id(""));
    CHECK(!ttsc::VoiceStore::valid_id(".hidden"));
    CHECK(!ttsc::VoiceStore::valid_id(".."));
    CHECK(!ttsc::VoiceStore::valid_id("a/b"));
    CHECK(!ttsc::VoiceStore::valid_id("a b"));
    CHECK(!ttsc::VoiceStore::valid_id(std::string(65, 'x')));
}
