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

#include "support/build_paths.hpp"  // #5010: the BEZEL_*_PATH / BEZELC_PATH aliases
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
using bezel::test::aot_detail::toolchainIdentityFrom;

using Digest = std::array<std::uint8_t, 32>;

// Distinguishable synthetic identities. Their exact values are irrelevant; what
// matters is only that they differ from one another.
Digest ident(std::uint8_t seed) {
    Digest d{};
    for (std::size_t i = 0; i < d.size(); ++i)
        d[i] = static_cast<std::uint8_t>(seed + i);
    return d;
}

// A stand-in toolchain identity (per #3006): the external clang/ld/libLLVM the
// cache key now folds in. Held CONSTANT in the existing object/link tests (so
// their compiler/source/mode/runtime/recipe assertions are unchanged) and
// VARIED in the new toolchain tests below.
const Digest kToolchain = ident(5);

const char kSourceA[] =
    "module M; class C public static method Main(): int return 0; end Main; "
    "end C;";
const char kSourceB[] =
    "module M; class C public static method Main(): int return 1; end Main; "
    "end C;";
// A program returning a DISTINCTIVE non-zero value, so an equivalence check
// between two builds of it (both must exit 42) is non-trivial.  It declares
// multiple enums and classes with methods/constructors so the pointer-keyed
// emission maps (methodFns_/ctorFns_/disposerFns_/supersOf) have enough entries
// for their iteration order to affect the .rodata layout — the exact hazard
// fix #3020 made deterministic (name-sorted emission).  A regression that
// restores unordered iteration would make two forked compiles of this source
// diverge by a few .rodata bytes (an enum-name / descriptor reorder), which the
// byte-equality assertion below catches.
const char kSourceDistinct[] =
    "module M; "
    "enum FileAccess Read, Write; end FileAccess; "
    "enum FileMode Append, Truncate; end FileMode; "
    "enum FileShare None, Read; end FileShare; "
    "class A public tag: int; "
    "  public constructor(t: int) tag := t; end constructor; "
    "  public method probe(o: Object): int => (if o is A then 1 else 0); "
    "end A; "
    "class B public tag: int; "
    "  public constructor(t: int) tag := t; end constructor; "
    "  public method probe(o: Object): int => (if o is B then 1 else 0); "
    "end B; "
    "public class C public static method Main(): int => 42; end C;";

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
// fix #2877: the §22.16 checked-contracts mode. `runAot(src)` defaults this
// bit ON and `runAot(src, CodegenOptions{})` leaves it OFF, so it is a mode the
// corpus pairs on ONE source — and until #2877 it was absent from both key
// composers.
std::string contractsMode() {
    CodegenOptions o;
    o.checkedContracts = true;
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
    BEZEL_EXPECT_TRUE(composeObjectKey(ident(1), plainMode(), kPrelude, kSourceA, kToolchain) !=
                      composeObjectKey(ident(2), plainMode(), kPrelude, kSourceA, kToolchain));
}

// A different program must never serve another program's object.
BEZEL_TEST(aot_cache_key, object_key_moves_when_the_source_moves) {
    BEZEL_EXPECT_TRUE(composeObjectKey(ident(1), plainMode(), kPrelude, kSourceA, kToolchain) !=
                      composeObjectKey(ident(1), plainMode(), kPrelude, kSourceB, kToolchain));
}

// Every CodegenOptions bit alters the emitted object, so each must key apart.
// Conflating the ASan and non-ASan products would be a false-green bug: an
// uninstrumented binary would be served to a test that asked for the memory
// net, and would report clean because nothing was watching.
BEZEL_TEST(aot_cache_key, object_key_separates_every_codegen_mode) {
    const std::set<std::string> keys{
        composeObjectKey(ident(1), plainMode(), kPrelude, kSourceA, kToolchain),
        composeObjectKey(ident(1), asanMode(), kPrelude, kSourceA, kToolchain),
        composeObjectKey(ident(1), checkedMode(), kPrelude, kSourceA, kToolchain),
        // fix #2877: checked-CONTRACTS is a fourth distinct artifact. It was
        // missing from this row and from both composers, which is how the
        // collision below survived.
        composeObjectKey(ident(1), contractsMode(), kPrelude, kSourceA, kToolchain),
    };
    BEZEL_EXPECT_EQ(keys.size(), static_cast<std::size_t>(4));
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
    BEZEL_EXPECT_EQ(composeObjectKey(ident(1), plainMode(), kPrelude, kSourceA, kToolchain),
                    composeObjectKey(ident(1), plainMode(), kPrelude, kSourceA, kToolchain));
    const std::string recipe = Driver::linkRecipeSignature(false);
    BEZEL_EXPECT_TRUE(composeLinkKey(ident(9), ident(1), recipe, kToolchain) !=
                      composeLinkKey(ident(9), ident(2), recipe, kToolchain));
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
        composeObjectKey(ident(1), plainMode(), kPrelude, kSourceA, kToolchain) !=
        composeObjectKey(ident(1), plainMode(), kPreludeChanged, kSourceA, kToolchain));
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
                                  plainMode(), driverPrelude, kSourceA,
                                  bezel::test::aot_detail::buildIdentity().toolchain));
    }
}

// ---------------------------------------------------------------------------
// TOOLCHAIN identity (per #3006): the external clang/ld/libLLVM. Before #3006
// neither key hashed the toolchain, so a Fedora clang/binutils/llvm-libs update
// left ~16,830 stored products servable against a recipe that no longer matched
// the toolchain that produced them. The toolchain digest is now folded into the
// OBJECT key (the emitted object depends on the libLLVM backend), the LINK key
// (the link runs clang/ld), and the whole-exe KE key. These pin all three.
//
// The composer is fed SYNTHETIC toolchain digests (ident(N)), mirroring how the
// tests above vary the compiler identity — the pure function makes the toolchain
// swappable without a real system update. The fail-closed property is pinned on
// toolchainIdentityFrom directly (the function the live gatherer feeds).
// ---------------------------------------------------------------------------

// A toolchain update (new clang/ld/libLLVM) MUST invalidate every cached object:
// the in-process codegen calls into libLLVM.so, so a backend change can emit
// different bytes for the same source. Without this the split would serve a
// stale object and link it into a stale executable — the false-green this whole
// file exists to prevent. (#3006.)
BEZEL_TEST(aot_cache_key, object_key_moves_when_the_toolchain_moves) {
    BEZEL_EXPECT_TRUE(
        composeObjectKey(ident(1), plainMode(), kPrelude, kSourceA, ident(5)) !=
        composeObjectKey(ident(1), plainMode(), kPrelude, kSourceA, ident(6)));
}

// The same holds for the LINK product: a clang/ld update can link different
// bytes from the same object, so the link key must carry the toolchain too.
BEZEL_TEST(aot_cache_key, link_key_moves_when_the_toolchain_moves) {
    const std::string recipe = Driver::linkRecipeSignature(false);
    BEZEL_EXPECT_TRUE(composeLinkKey(ident(9), ident(1), recipe, ident(5)) !=
                      composeLinkKey(ident(9), ident(1), recipe, ident(6)));
}

// Stability is the other half: a no-op rebuild (the COMMON case — every test
// iteration) MUST NOT move the key. The toolchain identity is stable across
// relinks of bezel_tests (the system clang/ld/libLLVM are untouched by a
// compiler rebuild), so two identical inputs reproduce the same key. If this
// failed, the cache would never warm and the fix would be useless.
BEZEL_TEST(aot_cache_key, toolchain_key_is_stable_across_rebuilds) {
    BEZEL_EXPECT_EQ(
        composeObjectKey(ident(1), plainMode(), kPrelude, kSourceA, ident(5)),
        composeObjectKey(ident(1), plainMode(), kPrelude, kSourceA, ident(5)));
    const std::string recipe = Driver::linkRecipeSignature(false);
    BEZEL_EXPECT_EQ(composeLinkKey(ident(9), ident(1), recipe, ident(5)),
                    composeLinkKey(ident(9), ident(1), recipe, ident(5)));
}

// FAIL CLOSED (Plan-A invariant 1.21/1.22). clang is the one tool the link
// cannot proceed without, so an unidentified clang ⇒ no sound toolchain
// identity ⇒ the cache disables itself for the process (returns no digest)
// rather than mint an under-keyed entry a pre-update toolchain could later
// stale-hit. Mirrors the existing fail-closed on an unreadable runtime archive.
// ld and libLLVM are BEST-EFFORT (optional) — their absence does NOT fail.
BEZEL_TEST(aot_cache_key, toolchain_identity_fails_closed_when_clang_is_absent) {
    Digest d{};
    // clang unidentified (empty version string) ⇒ fail CLOSED.
    BEZEL_EXPECT_TRUE(!toolchainIdentityFrom("", "ld 2.43", "/usr/lib64/libLLVM.so|99|1234", d));
    // clang present ⇒ succeeds even with ld/libLLVM absent (best-effort).
    BEZEL_EXPECT_TRUE(toolchainIdentityFrom("clang 19", "", "", d));
    // And the identity is DETERMINISTIC: same inputs ⇒ same digest (so two
    // processes on the same toolchain agree, and a stable key follows).
    Digest d2{};
    BEZEL_EXPECT_TRUE(toolchainIdentityFrom("clang 19", "", "", d2));
    BEZEL_EXPECT_TRUE(d == d2);
    // A different clang version ⇒ a different digest (so the key moves).
    Digest d3{};
    BEZEL_EXPECT_TRUE(toolchainIdentityFrom("clang 20", "", "", d3));
    BEZEL_EXPECT_TRUE(d != d3);
}

// The LIVE toolchain identity is computable in this environment (clang is on
// PATH for the link to work at all) and feeds the real keys. This is the
// end-to-end wiring check: the live build identity carries a non-zero
// toolchain digest, so computeObjectKey/computeLinkKey (which pass it through)
// actually depend on it.
BEZEL_TEST(aot_cache_key, live_identity_carries_a_toolchain_component) {
    const auto& id = bezel::test::aot_detail::buildIdentity();
    if (!id.usable) return;  // split unusable in this configuration — nothing to pin
    // clang is required for the cache to be usable at all (fail closed), so a
    // usable identity implies a non-zero toolchain digest: the live keys really
    // do carry the external toolchain, not just the compiler/runtime archives.
    bool anyNonZero = false;
    for (std::size_t i = 0; i < id.toolchain.size(); ++i)
        if (id.toolchain[i]) { anyNonZero = true; break; }
    BEZEL_EXPECT_TRUE(anyNonZero);
}

// ---------------------------------------------------------------------------
// LINK KEY (K2): object bytes ‖ runtime archive ‖ link recipe.
// ---------------------------------------------------------------------------

// The load-bearing new claim of the split. A runtime-only rebuild MUST
// invalidate every cached executable — otherwise a test would run a binary
// containing the OLD runtime and report that the new one works.
BEZEL_TEST(aot_cache_key, link_key_moves_when_the_runtime_moves) {
    const std::string recipe = Driver::linkRecipeSignature(false);
    BEZEL_EXPECT_TRUE(composeLinkKey(ident(9), ident(1), recipe, kToolchain) !=
                      composeLinkKey(ident(9), ident(2), recipe, kToolchain));
}

// Different generated code must never be served the same executable.
BEZEL_TEST(aot_cache_key, link_key_moves_when_the_object_moves) {
    const std::string recipe = Driver::linkRecipeSignature(false);
    BEZEL_EXPECT_TRUE(composeLinkKey(ident(8), ident(1), recipe, kToolchain) !=
                      composeLinkKey(ident(9), ident(1), recipe, kToolchain));
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
    BEZEL_EXPECT_TRUE(composeLinkKey(ident(9), ident(1), real, kToolchain) !=
                      composeLinkKey(ident(9), ident(1), edited, kToolchain));
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
    BEZEL_EXPECT_TRUE(composeLinkKey(ident(9), ident(1), plain, kToolchain) !=
                      composeLinkKey(ident(9), ident(1), asan, kToolchain));
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
        composeObjectKey(ident(1), plainMode(), kPrelude, kSourceA, kToolchain) !=
        composeLinkKey(ident(1), ident(1), Driver::linkRecipeSignature(false), kToolchain));
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

    CodegenOptions opts;
    const std::string obj = dir + "/prog.o";
    const std::string exe1 = dir + "/prog1";
    const std::string exe2 = dir + "/prog2";
    const std::string ccErr = dir + "/cc.err";
    // Fork-isolate each driver step. Every OTHER AOT test in the suite forks its
    // compile because the front-end has a live crash history (see
    // aot_crash_isolation_test.cpp — "without the fork, the SIGSEGV would abort
    // the whole bezel_tests binary, taking down every other test in the shard").
    // These two end-to-end cache-key tests were the lone paths that ran the
    // Driver in-process, so an in-process compile fault here could kill a shard;
    // for the split_build sibling it was also the #1608 flake source. Forking
    // each step in its own child removes that path. // per #1608
    auto run = [](const char* what,
                  const bezel::test::aot_detail::CompileStatus& cc) -> bool {
        if (cc.outcome != bezel::test::aot_detail::CompileOutcome::Ok)
            std::cout << "    " << what << " did not succeed: " << cc.diag << "\n";
        return cc.outcome == bezel::test::aot_detail::CompileOutcome::Ok;
    };
    const bool compiled =
        run("compileExecutableObject",
            bezel::test::aot_detail::forkedCompileObject(src, obj, opts, ccErr));
    BEZEL_EXPECT_TRUE(compiled);
    if (!compiled) return;
    BEZEL_EXPECT_TRUE(run("linkExecutableObject(exe1)",
                          bezel::test::aot_detail::forkedLink(
                              obj, exe1, opts, ccErr)));
    BEZEL_EXPECT_TRUE(run("linkExecutableObject(exe2)",
                          bezel::test::aot_detail::forkedLink(
                              obj, exe2, opts, ccErr)));

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

// The split path must be BYTE-FOR-BYTE EXACT: compiling+linking in ONE driver
// call (what bezelc ships, and what the harness does when the cache is off) and
// compiling then linking as TWO calls (what the cache does) must produce the
// SAME executable. This is what lets the cache be an optimisation rather than a
// second, weaker oracle: a cache HIT serves a binary the real compile would have
// produced.
//
// FORK-ISOLATED + DETERMINISTIC COMPILER (#1608 / #3020). Both builds compile
// the SAME source in separate forked children. The byte-equality assertion
// (hCombined == hSplit) was temporarily relaxed to observational equivalence
// (#1608 narrow) because the compiler emitted its rodata globals in POINTER-KEYED
// unordered_map iteration order — methodFns_ / ctorFns_ / disposerFns_ and the
// supersOf descriptor maps — whose bucket order varies with the forked child's
// inherited heap layout, making two forked compiles diverge by a few .rodata
// bytes (~15% flake). Fix #3020 made the emission DETERMINISTIC by name-sorting
// those maps' iteration (precedent: accessorSpecTagFor), plus fixed a latent
// order-dependence in the buildSpecializedMethodFn cache key that the name-sort
// exposed (a hasUnresolved check on the LLVM i32 type misclassified a genuine
// `int` arg as a placeholder fallback, desyncing genericSubst_ from
// genericSubstSemaTy_ and corrupting the specialization cache into an empty-idKey
// collision — the #2192 transitive-generic break). With both fixes the compiler
// is deterministic across processes/forks and the byte-equality pin is restored.
// // per #1608 / #3020
BEZEL_TEST(aot_cache_key, split_build_matches_the_combined_build_byte_for_byte) {
    const std::string dir = bezel::test::aot_detail::makeWorkDir();
    BEZEL_EXPECT_TRUE(!dir.empty());
    if (dir.empty()) return;
    bezel::test::aot_detail::WorkDir cleanup{dir};

    const std::string src = dir + "/prog.bzl";
    {
        std::ofstream os(src, std::ios::binary);
        os << kSourceDistinct;
    }

    CodegenOptions opts;
    const std::string ccErr = dir + "/cc.err";
    auto built = [](const char* what,
                    const bezel::test::aot_detail::CompileStatus& cc) -> bool {
        if (cc.outcome != bezel::test::aot_detail::CompileOutcome::Ok)
            std::cout << "    " << what << " did not succeed: " << cc.diag << "\n";
        return cc.outcome == bezel::test::aot_detail::CompileOutcome::Ok;
    };

    // COMBINED: compile+link in one driver call (the shipping bezelc path).
    const std::string combined = dir + "/combined";
    BEZEL_EXPECT_TRUE(built("compileFile(combined)",
                            bezel::test::aot_detail::forkedCompile(
                                src, combined, opts, ccErr)));
    // compileFile must clean up after itself — a left-behind object would be a
    // stray artifact in every bezelc invocation. The forked child removes it
    // before _Exit, so it is gone by the time the parent resumes.
    BEZEL_EXPECT_TRUE(!fs::exists(combined + ".o"));

    // SPLIT: compile then link as two driver calls (the cache's path).
    const std::string obj = dir + "/split.o";
    const std::string split = dir + "/split";
    BEZEL_EXPECT_TRUE(
        built("compileExecutableObject(split.o)",
              bezel::test::aot_detail::forkedCompileObject(src, obj, opts, ccErr)) &&
        built("linkExecutableObject(split)",
              bezel::test::aot_detail::forkedLink(obj, split, opts, ccErr)));

    // #3020: BYTE-FOR-BYTE equality. Two forked compiles of the same source must
    // now produce identical executables — the compiler's rodata emission is
    // deterministic (name-sorted), so the inherited-heap-layout divergence that
    // used to reorder a few globals is gone. A regression here means either the
    // name-sort was reverted or a new pointer-keyed emission map was added
    // without sorting.
    std::array<uint8_t, 32> hCombined{}, hSplit{};
    BEZEL_EXPECT_TRUE(sha256File(combined, hCombined));
    BEZEL_EXPECT_TRUE(sha256File(split, hSplit));
    if (hCombined != hSplit) {
        std::cout << "    BYTE DIVERGENCE: combined and split executables differ\n";
    }
    BEZEL_EXPECT_EQ(hCombined, hSplit);

    // Defense in depth: both must also RUN and exit 42 (the observational check
    // the #1608 narrow kept as the sole assertion — kept now as a second gate).
    auto exitCodeOf = [&](const std::string& exe,
                          const std::string& tag) -> int {
        const std::string out = dir + "/" + tag + ".out";
        const std::string err = dir + "/" + tag + ".err";
        bool timedOut = false;
        int status = bezel::test::aot_detail::spawnCaptured(
            {exe}, out, err, bezel::test::aot_detail::aotTimeoutSec(), timedOut, {});
        if (timedOut) {
            std::cout << "    " << tag << " timed out\n";
            return -1;
        }
        if (WIFSIGNALED(status)) {
            std::cout << "    " << tag << " killed by signal "
                      << WTERMSIG(status) << "\n";
            return -1;
        }
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    };
    BEZEL_EXPECT_EQ(exitCodeOf(combined, "combined"), 42);
    BEZEL_EXPECT_EQ(exitCodeOf(split, "split"), 42);
}

// ---------------------------------------------------------------------------
// #1608 regression pin — recompiling the same source is DETERMINISTIC.
// ---------------------------------------------------------------------------
// This looks like a tautology ("obviously the same source compiles to the same
// object"). It is NOT, and #1608 was the proof. The flake it pins was real and
// gate-burning: when two compiles of the same source ran IN-PROCESS (the old
// split_build shape), the first mutated process-global compiler state (LLVM
// context / name-to-pointer maps) that the second then read; co-resident
// runtime_* stress tests (round-robin'd into the same shard) dirtied the
// initial state further; the two emitted objects diverged by a few bytes; the
// linked executables got distinct GNU build-ids; and a content-equality
// assertion flaked on ~15% of gate shard runs. run_parallel's crash attribution
// names the LAST [ RUN ] line in the shard, so the failure was long misread as a
// builtin_registry_snapshot "crash" (those tests are pure constexpr reads and
// always passed — the gate log confirms [ OK ] on each).
//
// The fix was to fork-isolate each compile (forkedCompileObject) so every
// compile starts from the SAME parent state — the only sound basis for a byte-
// equality claim across recompiles. This test pins that property directly and
// narrowly: three independent forked compiles of one source must be byte-for-
// byte identical objects, every time, regardless of what prior co-resident
// tests left in the process. If this ever flakes or fails, the #1608 isolation
// has regressed — either an in-process compile path crept back in, or the
// compiler grew a new state-dependent non-determinism. Either is a shard-killer
// under the gate's parallel sharded run.
//
// SCOPE of this pin (#3020). It pins BACK-TO-BACK fork determinism: the three
// forks here run in quick succession, so each child inherits near-identical
// parent state and the emitted objects match. The deeper non-back-to-back fork
// case (two forks SEPARATED by other work, the split_build shape) is now ALSO
// pinned byte-for-byte by split_build_matches_the_combined_build_byte_for_byte
// above, since fix #3020 made the compiler's rodata emission deterministic
// (name-sorted). This test remains a narrow canary for the in-process-compile
// class. // per #1608/#3020
BEZEL_TEST(aot_cache_key, recompiling_same_source_is_deterministic_1608) {
    const std::string dir = bezel::test::aot_detail::makeWorkDir();
    BEZEL_EXPECT_TRUE(!dir.empty());
    if (dir.empty()) return;
    bezel::test::aot_detail::WorkDir cleanup{dir};

    const std::string src = dir + "/prog.bzl";
    {
        std::ofstream os(src, std::ios::binary);
        os << kSourceA;
    }
    CodegenOptions opts;
    const std::string ccErr = dir + "/cc.err";

    // Each call forks a fresh child from the (compile-unchanged) parent, so all
    // three compiles start from identical process state. An IN-PROCESS recompile
    // here would reproduce the #1608 flake (the second/third object would read
    // state the first mutated); the forked path cannot.
    auto hashOfForkedCompile = [&](const std::string& objPath,
                                   Digest& out) -> bool {
        const auto cc = bezel::test::aot_detail::forkedCompileObject(
            src, objPath, opts, ccErr);
        if (cc.outcome != bezel::test::aot_detail::CompileOutcome::Ok) {
            std::cout << "    forkedCompileObject did not succeed: " << cc.diag
                      << "\n";
            return false;
        }
        return sha256File(objPath, out);
    };

    Digest h1{}, h2{}, h3{};
    BEZEL_EXPECT_TRUE(hashOfForkedCompile(dir + "/a.o", h1));
    BEZEL_EXPECT_TRUE(hashOfForkedCompile(dir + "/b.o", h2));
    BEZEL_EXPECT_TRUE(hashOfForkedCompile(dir + "/c.o", h3));
    // The load-bearing pin: every forked compile of the same source is the same
    // object. (Three pairwise checks, not one, so a two-way coincidence is not
    // mistaken for determinism.)
    BEZEL_EXPECT_TRUE(h1 == h2);
    BEZEL_EXPECT_TRUE(h2 == h3);
    BEZEL_EXPECT_TRUE(h1 == h3);
}

// ---------------------------------------------------------------------------
// LINK PROVENANCE (#2877): the archive identity is trusted only for a binary
// the BUILD SYSTEM declared.
//
// #2877's confirmed mechanism: a binary hand-linked from the SAME archives plus
// one object that shadows an archive member is a DIFFERENT compiler, and the
// pre-fix identity could not see the difference — the k2 identity is
// SHA-256(core.a) ‖ SHA-256(runtime.a) ‖ toolchain, none of which is a function
// of the running binary's own code, and the STALE-CORE guard only compares
// mtimes. Measured on this worktree with two probes linking one archive, one of
// them with a shadowing src/sema/EntryPoint.cpp object:
//
//   pre-fix   plain and shadowed:  compiler f13bf580…  KE 594d7694…  K1 369b39a1…
//                                  IDENTICAL — whichever runs first mints the
//                                  entry and the other is served it.
//   post-fix  plain:               compiler 60c003cc…  (= its own exe hash)
//             shadowed:            compiler 4a2c29c2…  (= its own exe hash)
//                                  DISTINCT at every level.
//
// #3669's verify-on-read does NOT close this: it compares the entry's byte SIZE
// against its `.sz` sidecar, so it catches a TORN entry. A wrong-but-complete
// product has a consistent size and is served.
// ---------------------------------------------------------------------------

// THE SAFETY PIN, and the more important direction of the two. The fallback
// identity keys PER BINARY, so a guard that stopped recognising the REAL test
// binaries would split this shared store six ways and re-cool it on every
// test-only relink — a far larger cost than the defect. This row asserts the
// live guard says YES for the binary running it, and that the identity it then
// uses really is the archive's hash.
BEZEL_TEST(aot_cache_key, the_running_test_binary_is_declared_and_keys_on_the_archive_2877) {
    BEZEL_EXPECT_TRUE(bezel::test::aot_detail::runningExeIsADeclaredTestBinary());
#ifdef BEZEL_CORE_LIB_PATH
    const auto& id = bezel::test::aot_detail::buildIdentity();
    BEZEL_EXPECT_TRUE(id.usable);
    Digest core{};
    BEZEL_EXPECT_TRUE(sha256File(BEZEL_CORE_LIB_PATH, core));
    // If this fails the guard has false-negatived and the cache just went cold.
    BEZEL_EXPECT_TRUE(id.compiler == core);
#endif
}

// The manifest's location is DERIVED from BEZEL_CORE_LIB_PATH rather than passed
// as its own -D (a new definition would re-key every test TU, and the PCH's
// REUSE_FROM forces it to be identical across all six groups anyway). That makes
// the <build>/libbezel_core.a <-> <build>/tests/test_binaries.txt layout a
// coupling, and the guard FAILS OPEN — so if the layout moved, the guard would
// silently stop guarding and nothing else would notice. This row is what makes
// that impossible.
BEZEL_TEST(aot_cache_key, manifest_path_derivation_finds_the_real_cmake_manifest_2877) {
    const std::string p = bezel::test::aot_detail::testBinaryManifestPath();
    BEZEL_EXPECT_TRUE(!p.empty());
    std::ifstream in(p, std::ios::binary);
    BEZEL_EXPECT_TRUE(static_cast<bool>(in));
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    // "<group> TAB <path> TAB <#sources>" — at least one row, and this binary
    // is one of them.
    BEZEL_EXPECT_TRUE(text.find('\t') != std::string::npos);
    BEZEL_EXPECT_TRUE(
        bezel::test::aot_detail::manifestDeclaresExe(text, "/proc/self/exe"));
}

// THE DEFECT, at the level the fix operates on. A hand-linked probe is a file
// the build system never produced; it must not be mistaken for a declared
// binary and so must not inherit the archive's identity. The second half is
// what makes the first non-vacuous: the same path IS recognised once a manifest
// actually declares it, so the NO above is a real judgement and not a parse
// failure.
BEZEL_TEST(aot_cache_key, an_undeclared_binary_is_not_mistaken_for_a_declared_one_2877) {
    const std::string dir = bezel::test::aot_detail::makeWorkDir();
    BEZEL_EXPECT_TRUE(!dir.empty());
    if (dir.empty()) return;
    bezel::test::aot_detail::WorkDir cleanup{dir};

    // The stand-in for #2877's `t_parent`: a real file, linked by nothing.
    const std::string handLinked = dir + "/t_parent";
    { std::ofstream os(handLinked, std::ios::binary); os << "ELF-ish"; }

    std::ifstream in(bezel::test::aot_detail::testBinaryManifestPath(),
                     std::ios::binary);
    const std::string real((std::istreambuf_iterator<char>(in)), {});
    BEZEL_EXPECT_TRUE(!real.empty());
    BEZEL_EXPECT_TRUE(!bezel::test::aot_detail::manifestDeclaresExe(real, handLinked));

    const std::string declares = "support\t" + handLinked + "\t1\n";
    BEZEL_EXPECT_TRUE(bezel::test::aot_detail::manifestDeclaresExe(declares, handLinked));
}

// Matching is by INODE, not by path text, and both directions matter:
//   * a lane's build/ is a SYMLINK to /mnt/fast/lane-builds/<name>, so the
//     manifest's CMake spelling and /proc/self/exe's resolved path name one file
//     with different text — a string compare would false-negative EVERY lane and
//     cool the store;
//   * a byte-identical COPY of a declared binary is a DIFFERENT file, and a copy
//     is exactly the undeclared case (nothing links a copy from the archive).
BEZEL_TEST(aot_cache_key, manifest_matching_is_by_inode_not_by_path_text_2877) {
    const std::string dir = bezel::test::aot_detail::makeWorkDir();
    BEZEL_EXPECT_TRUE(!dir.empty());
    if (dir.empty()) return;
    bezel::test::aot_detail::WorkDir cleanup{dir};

    const std::string declared = dir + "/bezel_tests_x";
    { std::ofstream os(declared, std::ios::binary); os << "same bytes"; }
    const std::string manifest = "x\t" + declared + "\t1\n";

    std::error_code ec;
    const std::string viaLink = dir + "/alias";
    fs::create_hard_link(declared, viaLink, ec);
    BEZEL_EXPECT_TRUE(!ec);
    BEZEL_EXPECT_TRUE(bezel::test::aot_detail::manifestDeclaresExe(manifest, viaLink));

    const std::string copy = dir + "/copy";
    fs::copy_file(declared, copy, ec);
    BEZEL_EXPECT_TRUE(!ec);
    BEZEL_EXPECT_TRUE(!bezel::test::aot_detail::manifestDeclaresExe(manifest, copy));
}

// fs::equivalent REPORTS an error for a path that does not exist, so the parser
// must consult the error_code rather than the bool alone. A manifest row for a
// binary this build has not linked yet (a group whose target was never built)
// must neither match nor throw.
BEZEL_TEST(aot_cache_key, manifest_rows_for_absent_binaries_never_match_2877) {
    const std::string dir = bezel::test::aot_detail::makeWorkDir();
    BEZEL_EXPECT_TRUE(!dir.empty());
    if (dir.empty()) return;
    bezel::test::aot_detail::WorkDir cleanup{dir};

    const std::string present = dir + "/present";
    { std::ofstream os(present, std::ios::binary); os << "x"; }

    const std::string manifest = "gone\t" + dir + "/never_built\t7\n"
                                 "here\t" + present + "\t1\n";
    BEZEL_EXPECT_TRUE(
        !bezel::test::aot_detail::manifestDeclaresExe(manifest, dir + "/never_built"));
    // The absent row must not abort the scan before the row that does match.
    BEZEL_EXPECT_TRUE(bezel::test::aot_detail::manifestDeclaresExe(manifest, present));
    // Junk that is not a manifest row is skipped, not matched.
    BEZEL_EXPECT_TRUE(!bezel::test::aot_detail::manifestDeclaresExe("no tabs here\n", present));
    BEZEL_EXPECT_TRUE(!bezel::test::aot_detail::manifestDeclaresExe("", present));
}

// ---------------------------------------------------------------------------
// LINK-TIME PROVENANCE (#4529): the archive identity survives a no-op
// re-archive, and does not survive a real one.
//
// #2877 closed the over-ACCEPTING direction of the stale-core mtime ordering.
// The over-REJECTING direction failed the 87313e75 gate and is what these rows
// pin. Measured on that tree:
//
//   13:35:32.055  libbezel_core.a archived              (gate[2/5])
//   13:35:35.41   the six test binaries linked          <- ordering HOLDS
//   13:35:58.73   libbezel_core.a RE-archived by gate[3/5]'s `--target bezelc
//                 check-lit`, from 50 ccache-hit objects (84-180 ms each) with
//                 no file under src/ or tests/ touched since 13:30
//
// llvm-ar stores members uid/gid 0/0 at an epoch-0 timestamp, so re-archiving
// the same 50 objects reproduces the archive byte for byte (verified on that
// tree: sha256 b38429a4…). The re-archive moved the MTIME and not one byte of
// CONTENT — yet `coreTime <= exeTime` inverted, every shard fell to the
// per-binary exe identity, the shared store split six ways for the whole run,
// and the safety pin above failed on `id.compiler == core`.
//
// The build now records SHA-256(core.a) at link time in `<binary>.coreid`
// (tests/CMakeLists.txt), and that record decides.
// ---------------------------------------------------------------------------

// THE WIRING PIN, and the row that fails if the POST_BUILD step is ever
// dropped: the record must EXIST beside this binary, and must equal the digest
// of the archive on disk. The last assertion is the 87313e75 scenario stated
// against REAL data — a hostile mtime ordering (as if core.a had just been
// re-archived) with the real record and the real archive must still be trusted.
BEZEL_TEST(aot_cache_key, the_build_records_the_link_time_core_digest_beside_this_binary_4529) {
    const std::string p = bezel::test::aot_detail::linkTimeCoreDigestPath();
    BEZEL_EXPECT_TRUE(!p.empty());
    Digest recorded{};
    BEZEL_EXPECT_TRUE(bezel::test::aot_detail::readLinkTimeCoreDigest(p, recorded));
#ifdef BEZEL_CORE_LIB_PATH
    Digest core{};
    BEZEL_EXPECT_TRUE(sha256File(BEZEL_CORE_LIB_PATH, core));
    BEZEL_EXPECT_TRUE(recorded == core);
    // mtimeOrderingOk=false is the gate's state: core.a re-archived AFTER this
    // binary was linked, identical bytes. The record must carry the decision.
    BEZEL_EXPECT_TRUE(bezel::test::aot_detail::archiveIdentityIsTrusted(
        true, recorded, core, /*mtimeOrderingOk=*/false));
#endif
}

// THE DECISION, at the level the fix operates on. Both directions matter and
// they are not symmetric:
//   * a record that MATCHES overrides a hostile mtime ordering (the cooling
//     defect) — without this the whole shared store re-splits on every build
//     that re-archives core.a after linking the tests;
//   * a record that DIFFERS overrides a friendly one (the soundness half) —
//     a binary whose archive has been rebuilt under it must NOT key on that
//     archive however the timestamps happen to fall, and the ordering alone
//     cannot see that case at all.
BEZEL_TEST(aot_cache_key, a_link_time_record_decides_over_the_mtime_ordering_4529) {
    using bezel::test::aot_detail::archiveIdentityIsTrusted;
    const Digest linked = ident(11);
    const Digest other = ident(12);

    // Record present.
    BEZEL_EXPECT_TRUE(archiveIdentityIsTrusted(true, linked, linked, false));
    BEZEL_EXPECT_TRUE(archiveIdentityIsTrusted(true, linked, linked, true));
    BEZEL_EXPECT_TRUE(!archiveIdentityIsTrusted(true, linked, other, true));
    BEZEL_EXPECT_TRUE(!archiveIdentityIsTrusted(true, linked, other, false));

    // No record (a tree built before #4529, or a hand-linked binary): EXACTLY
    // the previous rule, so nothing trusted before this fix becomes untrusted
    // and nothing rejected before it becomes trusted.
    BEZEL_EXPECT_TRUE(archiveIdentityIsTrusted(false, linked, other, true));
    BEZEL_EXPECT_TRUE(!archiveIdentityIsTrusted(false, linked, linked, false));
}

// A record that cannot be parsed must read as NO RECORD — which falls back to
// the mtime ordering — and never as a match. Conflating the two would turn a
// truncated file (a half-written POST_BUILD, a full disk) into a blanket
// "trust the archive", i.e. the #2877 defect arriving through the fix for it.
BEZEL_TEST(aot_cache_key, a_corrupt_link_time_record_reads_as_no_record_4529) {
    using bezel::test::aot_detail::parseHexDigest;
    using bezel::test::aot_detail::readLinkTimeCoreDigest;
    Digest d{};

    // The shapes the writer can legitimately produce.
    const std::string hex(64, 'a');
    BEZEL_EXPECT_TRUE(parseHexDigest(hex, d));
    BEZEL_EXPECT_TRUE(parseHexDigest(hex + "\n", d));
    BEZEL_EXPECT_TRUE(parseHexDigest("  " + hex + "\n", d));
    BEZEL_EXPECT_TRUE(parseHexDigest(hex + "  /path/to/libbezel_core.a\n", d));
    BEZEL_EXPECT_TRUE(parseHexDigest(std::string(64, 'A'), d));  // upper case
    // …and the VALUE is the bytes, not merely "it parsed" — a parser that
    // returned true while leaving `out` untouched would make every comparison
    // in this file compare one zero digest with another.
    Digest all_ff{};
    for (auto& b : all_ff) b = 0xFF;
    Digest v{};
    BEZEL_EXPECT_TRUE(parseHexDigest(std::string(64, 'f'), v));
    BEZEL_EXPECT_TRUE(v == all_ff);
    BEZEL_EXPECT_TRUE(parseHexDigest(std::string(64, 'F'), v));
    BEZEL_EXPECT_TRUE(v == all_ff);

    // The shapes that must NOT parse.
    BEZEL_EXPECT_TRUE(!parseHexDigest("", d));
    BEZEL_EXPECT_TRUE(!parseHexDigest(std::string(63, 'a'), d));   // truncated
    BEZEL_EXPECT_TRUE(!parseHexDigest(std::string(65, 'a'), d));   // not SHA-256
    BEZEL_EXPECT_TRUE(!parseHexDigest(std::string(63, 'a') + "z", d));
    BEZEL_EXPECT_TRUE(!parseHexDigest("   \n", d));

    // A file that is not there is "no record", and so is a directory.
    const std::string dir = bezel::test::aot_detail::makeWorkDir();
    BEZEL_EXPECT_TRUE(!dir.empty());
    if (dir.empty()) return;
    bezel::test::aot_detail::WorkDir cleanup{dir};
    BEZEL_EXPECT_TRUE(!readLinkTimeCoreDigest(dir + "/absent.coreid", d));
    BEZEL_EXPECT_TRUE(!readLinkTimeCoreDigest("", d));
    BEZEL_EXPECT_TRUE(!readLinkTimeCoreDigest(dir, d));  // a directory
    { std::ofstream os(dir + "/torn.coreid", std::ios::binary); os << "deadbeef"; }
    BEZEL_EXPECT_TRUE(!readLinkTimeCoreDigest(dir + "/torn.coreid", d));
    // …and a well-formed one is read back EXACTLY, byte for byte. The pattern
    // is deliberately non-zero so a reader that returned true without writing
    // `out` would be caught rather than matching a default-constructed Digest.
    const std::string pattern = "0123456789abcdef0123456789abcdef"
                                "0123456789abcdef0123456789abcdef";
    { std::ofstream os(dir + "/good.coreid", std::ios::binary);
      os << pattern << "\n"; }
    Digest got{};
    BEZEL_EXPECT_TRUE(readLinkTimeCoreDigest(dir + "/good.coreid", got));
    Digest expected{};
    BEZEL_EXPECT_TRUE(parseHexDigest(pattern, expected));
    BEZEL_EXPECT_TRUE(got == expected);
    BEZEL_EXPECT_TRUE(got[0] == 0x01 && got[1] == 0x23 && got[31] == 0xef);
}

// ---------------------------------------------------------------------------
// BEHAVIOUR-CHANGING ENVIRONMENT (#2877, key component (e)).
//
// BEZEL_OWNERSHIP_LAMBDA_DATAFLOW and BEZEL_VERIFY_IR change what the
// in-process compiler DOES without moving a byte of any hashed artifact, so
// pre-#2877 a run that set one was served the product of a run that did not —
// and, the dangerous direction, a run that did not set one could be served a
// product minted by a run that did. BEZEL_AOT_CACHE_SALT is the operator escape
// hatch for the cases the provenance guard above cannot see.
// ---------------------------------------------------------------------------

// The conditional-fold half. A clean environment must contribute NO hash input,
// so that this component never moves a key on its own — not on this run and not
// on any future one. (It does NOT mean the keys are unchanged overall: the
// sibling `;cc=` fix moves every key deliberately. The two are separate
// decisions and only this one is about leaving keys alone.) Asserted on the
// pure seam AND through a real key, so an "almost no-op" — an empty separator,
// say — is caught rather than rounded to zero.
BEZEL_TEST(aot_cache_key, a_clean_environment_contributes_no_key_input_2877) {
    using bezel::test::aot_detail::withEnvironment;
    const std::string mode = plainMode();
    BEZEL_EXPECT_TRUE(withEnvironment(mode, "") == mode);
    BEZEL_EXPECT_TRUE(
        composeObjectKey(ident(1), withEnvironment(mode, ""), kPrelude, kSourceA, kToolchain) ==
        composeObjectKey(ident(1), mode, kPrelude, kSourceA, kToolchain));
    // And the live formatter appends via that same seam, so the mode text is
    // still exactly the documented schema when nothing is set. (The literal is
    // hardcoded on purpose — mm=1 because CodegenOptions::isMainModule defaults
    // to TRUE; comparing codegenModeString to itself would pass
    // however the two halves drifted.)
    CodegenOptions o;
    BEZEL_EXPECT_TRUE(
        bezel::test::aot_detail::codegenModeString(o) ==
        withEnvironment(std::string("k3o;sa=0;cd=0;cc=0;mm=1;sab=0;"),
                        bezel::test::aot_detail::cacheKeyEnvironment()));
}

// The isolation half. Each variable must move the key, and they must not
// collapse into one another — a salt that only distinguished "set" from "unset"
// would let two different experiments share a key, which is the defect again.
BEZEL_TEST(aot_cache_key, a_behaviour_changing_environment_moves_the_object_key_2877) {
    using bezel::test::aot_detail::withEnvironment;
    const std::string mode = plainMode();
    auto keyFor = [&](const char* env) {
        return composeObjectKey(ident(1), withEnvironment(mode, env), kPrelude,
                                kSourceA, kToolchain);
    };
    const std::string clean = keyFor("");
    const std::string salted = keyFor("BEZEL_AOT_CACHE_SALT=exp1;");
    const std::string salted2 = keyFor("BEZEL_AOT_CACHE_SALT=exp2;");
    const std::string dataflow = keyFor("BEZEL_OWNERSHIP_LAMBDA_DATAFLOW=1;");
    const std::string verify = keyFor("BEZEL_VERIFY_IR=1;");

    const std::set<std::string> distinct{clean, salted, salted2, dataflow, verify};
    BEZEL_EXPECT_TRUE(distinct.size() == 5);
}

// ---------------------------------------------------------------------------
// THE MODE BIT THAT WAS MISSING (#2877, via #4386/§22.16).
//
// `checkedContracts` reached CodegenOptions with #4386 and never reached either
// mode string, so the LIVE keys for the two §22.16 modes of one source were
// identical — measured on this tree before the fix, KE 03161ae5… and K1
// 540a926b… for both. The two forms that differ by exactly that bit
// (`runAot(src)`, which defaults it ON, and `runAot(src, CodegenOptions{})`,
// the release default) are designed to be paired on one source, so the corpus
// exercised the collision directly: whichever ran first minted the entry and
// the other was served it — a wrong product under a matching identity, reached
// by the ordinary suite and not by any hand-linked binary.
//
// It was not latent. tests/codegen/checked_contracts_4386_test.cpp pairs
// `satisfied_ensures_returns_normally_{with,without}_flag_spec_22_16` and
// `requires_violation_raises_{with,without}_flag_spec_22_16` on BYTE-IDENTICAL
// source — measured pre-fix, each pair's two modes shared one KE key and one K1
// key, so the second row to run was served the first row's binary. Both pairs
// happen to expect the same exit value, which is exactly why nobody noticed:
// the rows passed while proving nothing. The `without_flag` requires row is
// documented as "the load-bearing pin that proves the flag is NOT wired to
// `requires`" — and it could be answered by the WITH-flag binary.
//
// These rows go through the LIVE key functions, not the pure composers, because
// the defect was in what the live functions fed the composers: the composers
// were told two modes were one, and faithfully agreed.
// ---------------------------------------------------------------------------
BEZEL_TEST(aot_cache_key, live_keys_separate_the_two_checked_contracts_modes_2877) {
    const char* src = kSourceA;
    CodegenOptions off;                        // the §22.16 release default
    CodegenOptions on;  on.checkedContracts = true;   // what runAot(src) sets

    BEZEL_EXPECT_TRUE(bezel::test::aot_detail::computeCacheKey(src, off) !=
                      bezel::test::aot_detail::computeCacheKey(src, on));
    BEZEL_EXPECT_TRUE(bezel::test::aot_detail::computeObjectKey(src, off) !=
                      bezel::test::aot_detail::computeObjectKey(src, on));
    // Non-vacuous: the keys are real (an unusable identity would return "" for
    // both, and "" != "" is false — but an empty-vs-empty pair would then have
    // PASSED the two NE checks above had they been written as EQ, so pin
    // non-emptiness explicitly).
    BEZEL_EXPECT_TRUE(!bezel::test::aot_detail::computeCacheKey(src, off).empty());
    BEZEL_EXPECT_TRUE(!bezel::test::aot_detail::computeObjectKey(src, off).empty());
}

// THE RATCHET for the next forgotten bit. #4386 added an option and nothing
// failed; this file's mode rows all enumerate options by hand, so they grow
// only when someone remembers to grow them. A size check cannot be forgotten:
// any new CodegenOptions member changes it, and the build stops here with the
// instruction rather than silently serving one mode's product for another.
//
// Deliberately a static_assert, not a runtime row: a runtime row can be
// filtered out of a targeted lane run, whereas this cannot be run-and-skipped.
// Its REACH is a gate ratchet, not a desk one — it lives in a `support`-area
// TU, so neither `--target bezelc` nor `agent_check.sh --area codegen` compiles
// it, and someone adding an option in src/codegen will not see it fire until
// the support area is built. That is still strictly better than the
// hand-enumerated mode rows above, which fire for nobody.
static_assert(sizeof(CodegenOptions) == 5,
              "CodegenOptions changed shape. Every option that alters the "
              "emitted object or the link line MUST appear in BOTH "
              "aot_support.hpp modeString() and codegenModeString(), or the "
              "AOT cache serves one mode's product for another (#2877). Add "
              "it there, add a row to object_key_separates_every_codegen_mode "
              "above, then update this size.");
