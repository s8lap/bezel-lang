// Regression pin for issue 3009 (test-infra): the cache install path fsyncs the
// temp file's DATA before the atomic rename (aot_support.hpp durableRename), so a
// crash between the byte copy and the rename-in-cache-dir cannot leave a
// partial/truncated cache entry that a later lookup mistakes for whole. The
// storeFsyncs counter is the mechanism pin; the content-equality checks pin that
// the install itself stays whole.
//
// Fail-without / pass-with: durableRename fsyncs the temp fd before renaming;
// reverting it to a bare fs::rename makes the storeFsyncs assertion fail (zero
// fsyncs on the install path) — that is the mechanism this test exists to pin.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace fs = std::filesystem;
namespace aot = bezel::test::aot_detail;

namespace {

// Scratch dir unique to this test process (under the harness scratch root, so it
// lives on the NVMe, never the repo root or tmpfs /tmp).
std::string scratchDir() {
    static const std::string d =
        aot::scratchBase() + "/durability_" + std::to_string(::getpid());
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

// Read a whole file into a string (empty if missing/unreadable).
std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)), {});
}

}  // namespace

// #3009: durableRename fsyncs the temp's data, then atomically renames it into
// place. The destination holds the temp's exact bytes, AND an fsync ran on the
// install path (the storeFsyncs counter is the mechanism pin).
BEZEL_TEST(aot_cache_durability, durable_rename_fsyncs_and_preserves_content) {
    const std::string dir = scratchDir();
    const std::string tmp = dir + "/.tmp.payload";
    const std::string dst = dir + "/entry";
    const std::string payload = "cache entry bytes #3009";

    std::error_code ec;
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        BEZEL_EXPECT_TRUE(f.good());
        f.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        BEZEL_EXPECT_TRUE(f.good());
        f.close();
        BEZEL_EXPECT_TRUE(f.good());
    }

    const std::uint64_t fsyncs0 = aot::cacheStats().storeFsyncs.load();
    const std::error_code rec = aot::durableRename(tmp, dst);
    BEZEL_EXPECT_TRUE(!rec);                       // rename succeeded
    BEZEL_EXPECT_FALSE(fs::exists(tmp, ec));       // temp renamed away
    BEZEL_EXPECT_TRUE(fs::exists(dst, ec));        // destination present

    // Content is intact end-to-end (a truncated/partial entry would differ).
    BEZEL_EXPECT_EQ(readFile(dst), payload);

    // Mechanism pin: the data fsync ran on this install path (#3009). Reverting
    // durableRename to a bare fs::rename makes this fail (zero fsyncs recorded).
    BEZEL_EXPECT_TRUE(aot::cacheStats().storeFsyncs.load() > fsyncs0);
}

// #3009 end-to-end: a real storeToCache routes through durableRename, so the
// installed cache entry is whole and the install was fsync'd. Pinned via the
// storeFsyncs counter moving and the entry matching the source bytes.
BEZEL_TEST(aot_cache_durability, store_to_cache_routes_through_durable_rename) {
    const std::string dir = scratchDir();
    const std::string exe = dir + "/fake.exe";
    const std::string cacheSub = dir + "/cachesub";
    std::error_code ec;
    fs::create_directories(cacheSub, ec);
    const std::string payload = "exe-content-#3009";
    {
        std::ofstream f(exe, std::ios::binary | std::ios::trunc);
        BEZEL_EXPECT_TRUE(f.good());
        f.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        f.close();
        BEZEL_EXPECT_TRUE(f.good());
    }
    const std::string cachePath = cacheSub + "/hashed-entry";

    const std::uint64_t fsyncs0 = aot::cacheStats().storeFsyncs.load();
    const std::uint64_t fails0 = aot::cacheStats().storeFailures.load();
    aot::storeToCache(exe, cachePath);
    // A successful install fsyncs (mechanism) and records no failure.
    BEZEL_EXPECT_TRUE(aot::cacheStats().storeFsyncs.load() > fsyncs0);
    BEZEL_EXPECT_EQ(aot::cacheStats().storeFailures.load(), fails0);

    // The installed entry holds the exe's bytes whole.
    BEZEL_EXPECT_TRUE(fs::exists(cachePath, ec));
    BEZEL_EXPECT_EQ(readFile(cachePath), payload);
}
