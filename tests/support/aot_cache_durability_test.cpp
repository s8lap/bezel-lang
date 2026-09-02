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
//
// ---------------------------------------------------------------------------
// Issue 3669 (test-infra) layers an OPT-IN mode on top of that, and the two
// #3009 pins above are deliberately kept verbatim as the DEFAULT-mode contract.
//
// `BEZEL_AOT_STORE_FSYNC=0` trades the store's durability (~21k data fsyncs plus
// as many directory fsyncs per ASan gate pass, measured 2026-09-02) for
// VERIFY-ON-READ: such a store records the entry's byte size in a `<entry>.sz`
// sidecar, and installFromCache turns a size disagreement into a MISS —
// evicting entry and sidecar — so the property #3009 bought (a hit never runs a
// truncated executable) is preserved by verification instead of durability.
//
// What the 3669 rows pin, and why each can fail:
//   * durable_rename_skips_fsync_when_disabled — the mode reaches durableRename:
//     zero fsyncs, storeFsyncSkipped moves, content still intact.
//   * store_to_cache_skips_fsyncs_and_writes_sidecar_when_disabled — the mode is
//     ENV-driven end to end and the sidecar records the real size.
//   * default_mode_writes_no_sidecar_and_trusts_the_entry — the negative
//     control: with the env unset nothing changes, and an entry with no sidecar
//     is trusted exactly as before 3669 (a truncated one still installs).
//   * intact_entry_with_sidecar_installs_and_is_not_rejected — the positive
//     control, so the row below cannot pass merely because install always fails.
//   * truncated_entry_with_sidecar_is_a_verify_miss_and_is_evicted — the
//     FAIL-WITHOUT anchor for 3669. Before the fix installFromCache returns TRUE
//     on a truncated entry and hands it to the caller to RUN; with the fix it
//     returns false and the entry is gone. Neuter the verify-on-read block in
//     aot_support.hpp installFromCache and this row fails on all four of its
//     assertions.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <unistd.h>

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

// A fresh sub-directory per row, so no row can observe another's entries or
// sidecars (the rows evict files on purpose).
std::string caseDir(const std::string& name) {
    const std::string d = scratchDir() + "/" + name;
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

// Write `payload` to `path`, reporting a failed write rather than proceeding on
// a file whose contents are not what the row believes.
void writePayload(const std::string& path, const std::string& payload) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    BEZEL_EXPECT_TRUE(f.good());
    f.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    BEZEL_EXPECT_TRUE(f.good());
    f.close();
    BEZEL_EXPECT_TRUE(f.good());
}

// issue 3669: set (or clear) BEZEL_AOT_STORE_FSYNC for a scope and restore
// EXACTLY the prior state, including "it was unset" — these rows run inside a
// shard that shares its environment with every other test in the process, and a
// leaked BEZEL_AOT_STORE_FSYNC=0 would silently change the cache mode for all of
// them. aot_support.hpp reads the variable fresh on every call precisely so this
// works.
class StoreFsyncEnv {
public:
    // `value == nullptr` means "unset it".
    explicit StoreFsyncEnv(const char* value) {
        if (const char* prev = std::getenv(kName)) { had_ = true; prev_ = prev; }
        if (value) ::setenv(kName, value, 1); else ::unsetenv(kName);
    }
    ~StoreFsyncEnv() {
        if (had_) ::setenv(kName, prev_.c_str(), 1); else ::unsetenv(kName);
    }
    StoreFsyncEnv(const StoreFsyncEnv&) = delete;
    StoreFsyncEnv& operator=(const StoreFsyncEnv&) = delete;

private:
    static constexpr const char* kName = "BEZEL_AOT_STORE_FSYNC";
    bool had_ = false;
    std::string prev_;
};

}  // namespace

// #3009: durableRename fsyncs the temp's data, then atomically renames it into
// place. The destination holds the temp's exact bytes, AND an fsync ran on the
// install path (the storeFsyncs counter is the mechanism pin).
BEZEL_TEST(aot_cache_durability, durable_rename_fsyncs_and_preserves_content) {
    // issue 3669: pin the DEFAULT mode EXPLICITLY rather than ambiently. The
    // gate is expected to opt into BEZEL_AOT_STORE_FSYNC=0 in a later commit;
    // without this guard that flip would silently turn the #3009 fsync pins
    // below into no-ops (zero fsyncs is the correct behaviour in that mode), and
    // the regression they exist to catch would stop being tested with a green
    // tick. The env is restored on scope exit.
    const StoreFsyncEnv defaultMode(nullptr);

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
    // issue 3669: pin the DEFAULT mode EXPLICITLY rather than ambiently. The
    // gate is expected to opt into BEZEL_AOT_STORE_FSYNC=0 in a later commit;
    // without this guard that flip would silently turn the #3009 fsync pins
    // below into no-ops (zero fsyncs is the correct behaviour in that mode), and
    // the regression they exist to catch would stop being tested with a green
    // tick. The env is restored on scope exit.
    const StoreFsyncEnv defaultMode(nullptr);

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

// issue 3669: storeFsyncEnabled() is the ONE reading of the mode, and its
// default is the #3009 behaviour. Pinned directly because every other row in
// this file, and the whole default path of the cache, depends on "unset means
// fsync": a predicate that defaulted the other way would silently ship the
// no-fsync store to every lane.
BEZEL_TEST(aot_cache_durability, store_fsync_mode_defaults_to_enabled) {
    // The AMBIENT mode is whatever the shard was launched with: unset today,
    // but BEZEL_AOT_STORE_FSYNC=0 once issue 3690 adds it to the gate's
    // SHARD_ENV. Capture it and assert RESTORATION against it. Asserting
    // storeFsyncEnabled() is TRUE after the guards is a DIFFERENT claim — that
    // the ambient mode is the durable one — and it would make this row fail
    // under exactly the environment its own follow-up introduces. Measured:
    // BEZEL_AOT_STORE_FSYNC=0 turned this row and the sidecar row below red
    // while the rest of the file stayed green.
    const bool ambient = aot::storeFsyncEnabled();
    {
        const StoreFsyncEnv unset(nullptr);
        BEZEL_EXPECT_TRUE(aot::storeFsyncEnabled());   // unset => #3009 default
    }
    {
        const StoreFsyncEnv off("0");
        BEZEL_EXPECT_FALSE(aot::storeFsyncEnabled());  // "0" is the ONLY opt-out
    }
    {
        const StoreFsyncEnv on("1");
        BEZEL_EXPECT_TRUE(aot::storeFsyncEnabled());
    }
    {
        // Anything that is not exactly "0" keeps the durable store: a typo'd
        // value must not silently disable durability.
        const StoreFsyncEnv weird("00");
        BEZEL_EXPECT_TRUE(aot::storeFsyncEnabled());
    }
    // The guards restored the ambient environment, whatever it was.
    BEZEL_EXPECT_EQ(aot::storeFsyncEnabled(), ambient);
}

// issue 3669: the no-fsync mode reaches durableRename. Neither the data fsync
// (storeFsyncs) nor the directory fsync runs, storeFsyncSkipped records the
// trade, and the rename itself is unchanged — the destination still holds the
// temp's exact bytes.
BEZEL_TEST(aot_cache_durability, durable_rename_skips_fsync_when_disabled) {
    const std::string dir = caseDir("skip_fsync");
    const std::string tmp = dir + "/.tmp.payload";
    const std::string dst = dir + "/entry";
    const std::string payload = "cache entry bytes 3669 (no fsync)";
    writePayload(tmp, payload);

    const std::uint64_t fsyncs0 = aot::cacheStats().storeFsyncs.load();
    const std::uint64_t skipped0 = aot::cacheStats().storeFsyncSkipped.load();
    const std::error_code rec = aot::durableRename(tmp, dst, /*doFsync=*/false);

    std::error_code ec;
    BEZEL_EXPECT_TRUE(!rec);                     // rename still succeeded
    BEZEL_EXPECT_FALSE(fs::exists(tmp, ec));     // temp renamed away
    BEZEL_EXPECT_TRUE(fs::exists(dst, ec));      // destination present
    BEZEL_EXPECT_EQ(readFile(dst), payload);     // content intact

    // The mechanism: zero fsyncs, one recorded skip.
    BEZEL_EXPECT_EQ(aot::cacheStats().storeFsyncs.load(), fsyncs0);
    BEZEL_EXPECT_EQ(aot::cacheStats().storeFsyncSkipped.load(), skipped0 + 1);
}

// issue 3669 end to end: with BEZEL_AOT_STORE_FSYNC=0 in the environment a real
// storeToCache performs NO fsync and writes the `<entry>.sz` sidecar holding the
// entry's true byte size. The env is set through the same getenv the harness
// reads, so this row pins the ENV plumbing, not just the bool parameter.
BEZEL_TEST(aot_cache_durability,
           store_to_cache_skips_fsyncs_and_writes_sidecar_when_disabled) {
    const std::string dir = caseDir("nofsync_store");
    const std::string exe = dir + "/fake.exe";
    const std::string payload = "exe-content-3669-nofsync";
    writePayload(exe, payload);
    const std::string cachePath = dir + "/hashed-entry";

    // Captured for the restoration check below — see the note on
    // store_fsync_mode_defaults_to_enabled: the ambient mode is not necessarily
    // the durable one, and this row must hold under both.
    const bool ambient = aot::storeFsyncEnabled();
    const std::uint64_t fsyncs0 = aot::cacheStats().storeFsyncs.load();
    const std::uint64_t skipped0 = aot::cacheStats().storeFsyncSkipped.load();
    const std::uint64_t fails0 = aot::cacheStats().storeFailures.load();
    {
        const StoreFsyncEnv off("0");
        aot::storeToCache(exe, cachePath);
    }
    // The guard put the environment back, so no later test inherits the mode.
    BEZEL_EXPECT_EQ(aot::storeFsyncEnabled(), ambient);

    // No fsync ran on either the entry or its sidecar; both renames recorded a
    // skip (hence > rather than == skipped0 + 1).
    BEZEL_EXPECT_EQ(aot::cacheStats().storeFsyncs.load(), fsyncs0);
    BEZEL_EXPECT_TRUE(aot::cacheStats().storeFsyncSkipped.load() > skipped0);
    BEZEL_EXPECT_EQ(aot::cacheStats().storeFailures.load(), fails0);

    std::error_code ec;
    BEZEL_EXPECT_TRUE(fs::exists(cachePath, ec));
    BEZEL_EXPECT_EQ(readFile(cachePath), payload);

    // The sidecar exists and records the entry's REAL size (not, say, a stale
    // or zero value that would make verification vacuous).
    const std::string sidecar = aot::cacheSidecarPath(cachePath);
    BEZEL_EXPECT_TRUE(fs::exists(sidecar, ec));
    BEZEL_EXPECT_EQ(readFile(sidecar), std::to_string(payload.size()));
    BEZEL_EXPECT_EQ(fs::file_size(cachePath, ec), payload.size());
}

// issue 3669 NEGATIVE CONTROL — the default path is unchanged. With the env
// unset, storeToCache writes NO sidecar, so installFromCache has nothing to
// verify and trusts the entry exactly as it did before 3669: even a truncated
// entry installs, and verifyRejects does not move. This is what "the default
// behaviour stays today's" means as an executable assertion.
BEZEL_TEST(aot_cache_durability,
           default_mode_writes_no_sidecar_and_trusts_the_entry) {
    const std::string dir = caseDir("default_trust");
    const std::string exe = dir + "/fake.exe";
    const std::string payload = "exe-content-3669-default-mode";
    writePayload(exe, payload);
    const std::string cachePath = dir + "/hashed-entry";
    {
        const StoreFsyncEnv defaultMode(nullptr);
        aot::storeToCache(exe, cachePath);
    }

    std::error_code ec;
    const std::string sidecar = aot::cacheSidecarPath(cachePath);
    BEZEL_EXPECT_FALSE(fs::exists(sidecar, ec));   // no sidecar in fsync mode

    // Tear the entry, then install it. Pre-3669 behaviour: it is served.
    fs::resize_file(cachePath, 4, ec);
    BEZEL_EXPECT_TRUE(!ec);
    const std::uint64_t rejects0 = aot::cacheStats().verifyRejects.load();
    const std::string installed = dir + "/installed";
    BEZEL_EXPECT_TRUE(aot::installFromCache(cachePath, installed));
    BEZEL_EXPECT_EQ(aot::cacheStats().verifyRejects.load(), rejects0);
    BEZEL_EXPECT_TRUE(fs::exists(cachePath, ec));   // NOT evicted
    BEZEL_EXPECT_TRUE(fs::exists(installed, ec));
}

// issue 3669 POSITIVE CONTROL for the row below: an INTACT entry that carries a
// sidecar installs normally and is not rejected. Without this, the truncation
// row could pass merely because installFromCache had started failing for every
// entry — the loud-to-quiet failure mode in reverse.
BEZEL_TEST(aot_cache_durability,
           intact_entry_with_sidecar_installs_and_is_not_rejected) {
    const std::string dir = caseDir("intact_sidecar");
    const std::string exe = dir + "/fake.exe";
    const std::string payload = "exe-content-3669-intact-entry";
    writePayload(exe, payload);
    const std::string cachePath = dir + "/hashed-entry";
    {
        const StoreFsyncEnv off("0");
        aot::storeToCache(exe, cachePath);
    }

    std::error_code ec;
    const std::string sidecar = aot::cacheSidecarPath(cachePath);
    BEZEL_EXPECT_TRUE(fs::exists(sidecar, ec));

    const std::uint64_t rejects0 = aot::cacheStats().verifyRejects.load();
    const std::string installed = dir + "/installed";
    BEZEL_EXPECT_TRUE(aot::installFromCache(cachePath, installed));
    BEZEL_EXPECT_EQ(aot::cacheStats().verifyRejects.load(), rejects0);
    BEZEL_EXPECT_EQ(readFile(installed), payload);
    BEZEL_EXPECT_TRUE(fs::exists(cachePath, ec));   // entry survives a good read
    BEZEL_EXPECT_TRUE(fs::exists(sidecar, ec));
}

// issue 3669 FAIL-WITHOUT ANCHOR. Store an entry in no-fsync mode (so it carries
// a size sidecar), TRUNCATE it — exactly what a power cut without the #3009
// fsync can leave — and read it back. installFromCache must report a MISS and
// evict both files, so the caller runs the real compile instead of executing a
// short executable.
//
// Before this fix installFromCache returns TRUE here and hands the truncated
// entry to the caller to RUN; the caller cannot tell. Neutering the verify-on-
// read block in aot_support.hpp installFromCache fails all four assertions.
BEZEL_TEST(aot_cache_durability,
           truncated_entry_with_sidecar_is_a_verify_miss_and_is_evicted) {
    const std::string dir = caseDir("verify_reject");
    const std::string exe = dir + "/fake.exe";
    const std::string payload = "exe-content-3669-verify-on-read-anchor";
    writePayload(exe, payload);
    const std::string cachePath = dir + "/hashed-entry";
    {
        const StoreFsyncEnv off("0");
        aot::storeToCache(exe, cachePath);
    }

    std::error_code ec;
    const std::string sidecar = aot::cacheSidecarPath(cachePath);
    BEZEL_EXPECT_TRUE(fs::exists(sidecar, ec));
    BEZEL_EXPECT_EQ(readFile(sidecar), std::to_string(payload.size()));

    // Tear it. The entry is a hardlink of `exe`, so both shrink — which is
    // precisely the shape of a short entry at a valid key.
    fs::resize_file(cachePath, payload.size() - 5, ec);
    BEZEL_EXPECT_TRUE(!ec);
    BEZEL_EXPECT_EQ(fs::file_size(cachePath, ec), payload.size() - 5);

    const std::uint64_t rejects0 = aot::cacheStats().verifyRejects.load();
    const std::string installed = dir + "/installed";
    BEZEL_EXPECT_FALSE(aot::installFromCache(cachePath, installed));

    BEZEL_EXPECT_EQ(aot::cacheStats().verifyRejects.load(), rejects0 + 1);
    BEZEL_EXPECT_FALSE(fs::exists(cachePath, ec));   // torn entry evicted
    BEZEL_EXPECT_FALSE(fs::exists(sidecar, ec));     // its sidecar with it
    BEZEL_EXPECT_FALSE(fs::exists(installed, ec));   // nothing was installed
}

// issue 3669: a MALFORMED or empty sidecar is not a claim about anything, so the
// entry is trusted rather than evicted. A parse that failed closed would turn
// every unreadable sidecar into a cache miss — and, worse, delete a good entry.
BEZEL_TEST(aot_cache_durability, unparseable_sidecar_leaves_the_entry_trusted) {
    const std::string dir = caseDir("bad_sidecar");
    const std::string exe = dir + "/fake.exe";
    const std::string payload = "exe-content-3669-bad-sidecar";
    writePayload(exe, payload);
    const std::string cachePath = dir + "/hashed-entry";
    {
        const StoreFsyncEnv off("0");
        aot::storeToCache(exe, cachePath);
    }
    const std::string sidecar = aot::cacheSidecarPath(cachePath);
    writePayload(sidecar, "not-a-number");
    BEZEL_EXPECT_FALSE(aot::readSizeSidecar(sidecar).has_value());

    std::error_code ec;
    const std::uint64_t rejects0 = aot::cacheStats().verifyRejects.load();
    const std::string installed = dir + "/installed";
    BEZEL_EXPECT_TRUE(aot::installFromCache(cachePath, installed));
    BEZEL_EXPECT_EQ(aot::cacheStats().verifyRejects.load(), rejects0);
    BEZEL_EXPECT_TRUE(fs::exists(cachePath, ec));
    BEZEL_EXPECT_EQ(readFile(installed), payload);
}
