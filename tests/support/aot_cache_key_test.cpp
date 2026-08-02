// Correctness pins for the AOT build-product cache's KEYS (aot_support.hpp,
// the "k3" two-level split).
//
// WHY THIS FILE EXISTS. This cache does not make the suite wrong when it is
// slow; it makes the suite LIE when it is wrong. A key that fails to move when
// one of its build's inputs moves serves a binary produced by a DIFFERENT
// compiler or from DIFFERENT source, and the whole suite then reports success
// while validating the wrong thing — a false green, which is strictly worse
// than any cold gate the cache exists to avoid. The keys are therefore pinned
// here as a property test, not left to review.
//
// The build has exactly two steps and each has its own inputs:
//
//   object = compileExecutableObject(source)  <- compiler, codegen opts, the
//                                                auto-imported stdlib PRELUDE,
//                                                source
//   exe    = linkExecutableObject(object)     <- object bytes, runtime archive,
//                                                link recipe
//
// The tests below vary ONE input at a time and assert both directions: the key
// of the step that consumes it MUST move, and the key of the step that does not
// consume it MUST NOT (that second half is what buys the warm cache; without it
// the split would be pointless, with it wrong the split would be unsound).
//
// The key-composition functions are pure (composeObjectKey / composeLinkKey),
// so these assertions need no doctored archive on disk and are deterministic.
// The last two tests close the loop on the real toolchain: they check that
// relinking one object twice is reproducible, and that compiling+linking as two
// driver calls reproduces the shipping single call byte-for-byte.

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include <unistd.h>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

namespace {

namespace fs = std::filesystem;
using bezel::Driver;
using bezel::codegen::CodegenOptions;
using bezel::test::aot_detail::composeLinkKey;
using bezel::test::aot_detail::composeObjectKey;
using bezel::test::aot_detail::sha256File;

using Digest = std::array<std::uint8_t, 32>;

// Distinguishable synthetic identities. Their exact values are irrelevant; what
// matters is only that they differ from one another.
Digest ident(std::uint8_t seed) {
    Digest d{};
    for (std::size_t i = 0; i < d.size(); ++i)
        d[i] = static_cast<std::uint8_t>(seed + i);
    return d;
}

const char kSourceA[] =
    "module M; class C public static method Main(): int return 0; end Main; "
    "end C;";
const char kSourceB[] =
    "module M; class C public static method Main(): int return 1; end Main; "
    "end C;";

// A stand-in for the auto-imported stdlib prelude. The real text comes from
// Driver::preludeSourceFingerprint; these tests only need two distinguishable
// values, so they use literals and let the wiring test below check that the
// harness actually feeds the driver's own text into the key.
const char kPrelude[] = "module __p__; public enum E: byte A, B; end E;";
const char kPreludeChanged[] =
    "module __p__; public enum E: byte A, B, C; end E;";

// The codegen-mode strings the harness actually mints, obtained from the
// harness's own formatter so the test never hardcodes its text.
std::string plainMode() {
    return bezel::test::aot_detail::codegenModeString(CodegenOptions{});
}
std::string asanMode() {
    CodegenOptions o;
    o.sanitizeAddress = true;
    return bezel::test::aot_detail::codegenModeString(o);
}
std::string checkedMode() {
    CodegenOptions o;
    o.checkedDisposal = true;
    return bezel::test::aot_detail::codegenModeString(o);
}

}  // namespace

// ---------------------------------------------------------------------------
// OBJECT KEY (K1): compiler ‖ codegen mode ‖ stdlib prelude ‖ source
// — and NOTHING else.
// ---------------------------------------------------------------------------

// A different compiler must never serve another compiler's object. This is the
// single most important property in the file: it is what makes "the suite
// validated THIS compiler" true.
BEZEL_TEST(aot_cache_key, object_key_moves_when_the_compiler_moves) {
    BEZEL_EXPECT_TRUE(composeObjectKey(ident(1), plainMode(), kPrelude, kSourceA) !=
                      composeObjectKey(ident(2), plainMode(), kPrelude, kSourceA));
}

// A different program must never serve another program's object.
BEZEL_TEST(aot_cache_key, object_key_moves_when_the_source_moves) {
    BEZEL_EXPECT_TRUE(composeObjectKey(ident(1), plainMode(), kPrelude, kSourceA) !=
                      composeObjectKey(ident(1), plainMode(), kPrelude, kSourceB));
}

// Every CodegenOptions bit alters the emitted object, so each must key apart.
// Conflating the ASan and non-ASan products would be a false-green bug: an
// uninstrumented binary would be served to a test that asked for the memory
// net, and would report clean because nothing was watching.
BEZEL_TEST(aot_cache_key, object_key_separates_every_codegen_mode) {
    const std::set<std::string> keys{
        composeObjectKey(ident(1), plainMode(), kPrelude, kSourceA),
        composeObjectKey(ident(1), asanMode(), kPrelude, kSourceA),
        composeObjectKey(ident(1), checkedMode(), kPrelude, kSourceA),
    };
    BEZEL_EXPECT_EQ(keys.size(), static_cast<std::size_t>(3));
}

// The object key must NOT depend on the runtime archive or the link recipe:
// codegen consults neither (nothing under src/codegen, src/sema, src/ownership
// or src/module includes a runtime header or names BEZEL_RUNTIME_LIB_PATH —
// the archive reaches the build only through Driver's link command). That
// independence is what keeps every cached object valid across a runtime-only
// rebuild. The composer has no runtime or recipe parameter at all, which is the
// structural form of the claim; this pins that the LINK key does move with the
// runtime, so the two halves genuinely separate rather than both being blind.
BEZEL_TEST(aot_cache_key, object_key_is_independent_of_runtime_and_recipe) {
    BEZEL_EXPECT_EQ(composeObjectKey(ident(1), plainMode(), kPrelude, kSourceA),
                    composeObjectKey(ident(1), plainMode(), kPrelude, kSourceA));
    const std::string recipe = Driver::linkRecipeSignature(false);
    BEZEL_EXPECT_TRUE(composeLinkKey(ident(9), ident(1), recipe) !=
                      composeLinkKey(ident(9), ident(2), recipe));
}

// The auto-imported stdlib PRELUDE (§2.20 Result<T,E>, §7.1/§7.2 System.IO
// enums) is parsed into EVERY compilation unit, so it is as much an input to the
// emitted object as the user's own source: change an injected enum's members and
// programs using it emit different code. A cache that did not key on it would
// serve objects built against a stale prelude — a wrong binary reported as a
// pass, the exact false-green class this file exists to prevent.
//
// Today the prelude also lives inside the compiler archive `compilerId` hashes,
// so this is belt-and-braces — but only because the text happens to be a
// compiled-in constant. A refactor that loaded the stdlib from DISK would drop
// that coverage silently. This test fails if the explicit keying is ever
// removed, so the property cannot regress unnoticed.
BEZEL_TEST(aot_cache_key, object_key_moves_when_the_stdlib_prelude_moves) {
    BEZEL_EXPECT_TRUE(
        composeObjectKey(ident(1), plainMode(), kPrelude, kSourceA) !=
        composeObjectKey(ident(1), plainMode(), kPreludeChanged, kSourceA));
}

// …and the key the harness actually mints must use the DRIVER's prelude text,
// not some stale copy of it. Without this, the test above could pass while the
// live key fed a constant that no longer matched what the driver injects.
BEZEL_TEST(aot_cache_key, live_object_key_uses_the_drivers_prelude_text) {
    const std::string driverPrelude = Driver::preludeSourceFingerprint();
    BEZEL_EXPECT_TRUE(!driverPrelude.empty());
    // The §2.20 and §7.1/§7.2 preludes are both present in the fingerprint.
    BEZEL_EXPECT_TRUE(driverPrelude.find("choice Result<T, E>") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(driverPrelude.find("enum FileMode") != std::string::npos);
    // The memoized value the key uses is that same text.
    BEZEL_EXPECT_EQ(bezel::test::aot_detail::preludeFingerprint(),
                    driverPrelude);
    // And the live key is exactly the composition over it — so a prelude edit
    // moves the real key, not merely the synthetic one above.
    CodegenOptions plain;
    const std::string live =
        bezel::test::aot_detail::computeObjectKey(kSourceA, plain);
    if (!live.empty()) {  // empty only where the split is unusable
        BEZEL_EXPECT_EQ(live, composeObjectKey(
                                  bezel::test::aot_detail::buildIdentity().compiler,
                                  plainMode(), driverPrelude, kSourceA));
    }
}

// ---------------------------------------------------------------------------
// LINK KEY (K2): object bytes ‖ runtime archive ‖ link recipe.
// ---------------------------------------------------------------------------

// The load-bearing new claim of the split. A runtime-only rebuild MUST
// invalidate every cached executable — otherwise a test would run a binary
// containing the OLD runtime and report that the new one works.
BEZEL_TEST(aot_cache_key, link_key_moves_when_the_runtime_moves) {
    const std::string recipe = Driver::linkRecipeSignature(false);
    BEZEL_EXPECT_TRUE(composeLinkKey(ident(9), ident(1), recipe) !=
                      composeLinkKey(ident(9), ident(2), recipe));
}

// Different generated code must never be served the same executable.
BEZEL_TEST(aot_cache_key, link_key_moves_when_the_object_moves) {
    const std::string recipe = Driver::linkRecipeSignature(false);
    BEZEL_EXPECT_TRUE(composeLinkKey(ident(8), ident(1), recipe) !=
                      composeLinkKey(ident(9), ident(1), recipe));
}

// The link key deliberately does NOT contain the compiler identity — that is
// what keeps links warm across a codegen landing. The compiler still reaches
// this key through the two channels that can actually change the linked bytes:
// the object it emitted, and the link RECIPE it would run. This pins the recipe
// channel, i.e. that an edit to Driver's link command line invalidates cached
// executables even when the object is untouched.
BEZEL_TEST(aot_cache_key, link_key_moves_when_the_recipe_moves) {
    const std::string real = Driver::linkRecipeSignature(false);
    const std::string edited = real + " -lsomethingnew";
    BEZEL_EXPECT_TRUE(composeLinkKey(ident(9), ident(1), real) !=
                      composeLinkKey(ident(9), ident(1), edited));
}

// ASan and plain products must be distinct entries. They are separated TWICE
// over — the objects differ (instrumented vs not, pinned above) and the recipe
// differs (`-fsanitize=address -g`) — and this pins the second, so the
// separation survives even a hypothetical source whose two objects coincided.
BEZEL_TEST(aot_cache_key, link_recipe_separates_asan_from_plain) {
    const std::string plain = Driver::linkRecipeSignature(false);
    const std::string asan = Driver::linkRecipeSignature(true);
    BEZEL_EXPECT_TRUE(plain != asan);
    BEZEL_EXPECT_TRUE(asan.find("-fsanitize=address") != std::string::npos);
    BEZEL_EXPECT_TRUE(plain.find("-fsanitize=address") == std::string::npos);
    BEZEL_EXPECT_TRUE(composeLinkKey(ident(9), ident(1), plain) !=
                      composeLinkKey(ident(9), ident(1), asan));
}

// The recipe fingerprint must abstract the per-invocation paths (or no two
// tests could ever share a link product) while keeping every flag verbatim.
BEZEL_TEST(aot_cache_key, link_recipe_signature_is_path_independent_but_flag_exact) {
    const std::string sig = Driver::linkRecipeSignature(false);
    BEZEL_EXPECT_TRUE(sig.find("<OBJ>") != std::string::npos);
    BEZEL_EXPECT_TRUE(sig.find("<EXE>") != std::string::npos);
    BEZEL_EXPECT_TRUE(sig.find("clang") != std::string::npos);
    BEZEL_EXPECT_TRUE(sig.find("-lstdc++") != std::string::npos);
    // The runtime archive is keyed by CONTENT, so only its PRESENCE is recorded.
    const std::string rt = Driver::runtimeLibraryPath();
    if (!rt.empty()) {
        BEZEL_EXPECT_TRUE(sig.find("<RUNTIME>") != std::string::npos);
        BEZEL_EXPECT_TRUE(sig.find(rt) == std::string::npos);
    }
    // It must be the command that actually runs, modulo those paths: the same
    // builder produces both, so substituting the placeholders reproduces it.
    BEZEL_EXPECT_EQ(sig, Driver::linkCommandLine(
                             "<OBJ>", "<EXE>", /*extraObjects=*/{},
                             /*sanitizeAddress=*/false,
                             rt.empty() ? std::string() : std::string("<RUNTIME>")));
}

// ---------------------------------------------------------------------------
// CROSS-LEVEL: an object key and a link key can never name the same entry.
// ---------------------------------------------------------------------------
BEZEL_TEST(aot_cache_key, object_and_link_keys_never_collide) {
    // Feed both composers the most similar inputs available and require the
    // outputs to differ; the "k3l;" tag on the link key guarantees it.
    BEZEL_EXPECT_TRUE(
        composeObjectKey(ident(1), plainMode(), kPrelude, kSourceA) !=
        composeLinkKey(ident(1), ident(1), Driver::linkRecipeSignature(false)));
}

// ---------------------------------------------------------------------------
// END-TO-END on the real toolchain.
// ---------------------------------------------------------------------------

// The assumption a link-cache HIT makes: linking the same object against the
// same runtime with the same recipe twice yields the same executable. If this
// stopped holding, a hit would serve a binary the second link would not have
// produced.
BEZEL_TEST(aot_cache_key, relinking_the_same_object_is_reproducible) {
    const std::string dir = bezel::test::aot_detail::makeWorkDir();
    BEZEL_EXPECT_TRUE(!dir.empty());
    if (dir.empty()) return;
    bezel::test::aot_detail::WorkDir cleanup{dir};

    const std::string src = dir + "/prog.bzl";
    {
        std::ofstream os(src, std::ios::binary);
        os << kSourceA;
    }

    Driver driver;
    const std::string obj = dir + "/prog.o";
    CodegenOptions opts;
    const bool compiled =
        driver.compileExecutableObject(src, obj, /*deps=*/{}, opts).success;
    BEZEL_EXPECT_TRUE(compiled);
    if (!compiled) return;

    const std::string exe1 = dir + "/prog1";
    const std::string exe2 = dir + "/prog2";
    BEZEL_EXPECT_TRUE(driver.linkExecutableObject(obj, exe1, /*deps=*/{}, opts));
    BEZEL_EXPECT_TRUE(driver.linkExecutableObject(obj, exe2, /*deps=*/{}, opts));

    Digest h1{}, h2{};
    BEZEL_EXPECT_TRUE(sha256File(exe1, h1));
    BEZEL_EXPECT_TRUE(sha256File(exe2, h2));
    // A link-cache hit serves a binary the real link would not have produced if
    // this fails.
    BEZEL_EXPECT_TRUE(h1 == h2);
}

// END-TO-END through runAot: the LINK cache is genuinely reached, and serving
// from it is behaviour-preserving. Two programs that differ only inside a
// comment compile to byte-identical objects, so the second one's link key hits
// while its object key misses — the exact path a codegen landing takes for
// every test it did not affect (the front-end re-runs; the link is served).
//
// Both programs must still return their OWN result. That is the load-bearing
// half: it is what distinguishes "the same object legitimately links to the
// same executable" from "a stale binary was served".
BEZEL_TEST(aot_cache_key, link_cache_is_reached_and_preserves_behaviour) {
    if (!bezel::test::aot_detail::cacheEnabled()) return;
    if (bezel::test::aot_detail::computeObjectKey("probe", CodegenOptions{})
            .empty())
        return;  // split unusable in this configuration — nothing to pin

    auto& st = bezel::test::aot_detail::cacheStats();
    // A nonce keeps this run independent of anything a prior process cached.
    const std::string nonce = std::to_string(::getpid()) + "_" +
                              std::to_string(__LINE__);
    auto prog = [&](const std::string& mark) {
        return "module M; /* " + nonce + mark +
               " */ class C public static method Main(): int "
               "  return 23; end Main; end C;";
    };

    // Prime the store: first program builds for real (object + link both new).
    bezel::test::JitResult r1 = bezel::test::runAot(prog("a"));
    BEZEL_EXPECT_TRUE(r1.ran);
    BEZEL_EXPECT_EQ(r1.exitCode, 23);

    // Second program: DIFFERENT source (so a different object key and a
    // different whole-executable key) but the SAME emitted object, because the
    // only difference lives in a comment.
    const std::uint64_t o0 = st.objHits.load(), l0 = st.linkHits.load();
    bezel::test::JitResult r2 = bezel::test::runAot(prog("b"));
    BEZEL_EXPECT_TRUE(r2.ran);
    BEZEL_EXPECT_EQ(r2.exitCode, 23);           // correct result, not a stale one
    BEZEL_EXPECT_EQ(st.objHits.load(), o0);     // its object was compiled fresh
    BEZEL_EXPECT_EQ(st.linkHits.load(), l0 + 1);  // and its LINK was served
}

// The split path must be OBSERVATIONALLY EXACT: compiling+linking in ONE driver
// call (what bezelc ships, and what the harness does when the cache is off) and
// compiling then linking as TWO calls (what the cache does) must produce the
// same executable. This is what lets the cache be an optimisation rather than a
// second, weaker oracle.
BEZEL_TEST(aot_cache_key, split_build_matches_the_combined_build_byte_for_byte) {
    const std::string dir = bezel::test::aot_detail::makeWorkDir();
    BEZEL_EXPECT_TRUE(!dir.empty());
    if (dir.empty()) return;
    bezel::test::aot_detail::WorkDir cleanup{dir};

    const std::string src = dir + "/prog.bzl";
    {
        std::ofstream os(src, std::ios::binary);
        os << kSourceA;
    }

    Driver driver;
    CodegenOptions opts;

    const std::string combined = dir + "/combined";
    const bool builtCombined =
        driver.compileFile(src, combined, /*deps=*/{}, opts).success;
    BEZEL_EXPECT_TRUE(builtCombined);
    if (!builtCombined) return;
    // compileFile must clean up after itself — a left-behind object would be a
    // stray artifact in every bezelc invocation.
    BEZEL_EXPECT_TRUE(!fs::exists(combined + ".o"));

    const std::string obj = dir + "/split.o";
    const std::string split = dir + "/split";
    const bool builtSplit =
        driver.compileExecutableObject(src, obj, /*deps=*/{}, opts).success &&
        driver.linkExecutableObject(obj, split, /*deps=*/{}, opts);
    BEZEL_EXPECT_TRUE(builtSplit);
    if (!builtSplit) return;

    Digest hCombined{}, hSplit{};
    BEZEL_EXPECT_TRUE(sha256File(combined, hCombined));
    BEZEL_EXPECT_TRUE(sha256File(split, hSplit));
    // If this fails the cache is a DIFFERENT oracle from the shipping path.
    BEZEL_EXPECT_TRUE(hCombined == hSplit);
}
