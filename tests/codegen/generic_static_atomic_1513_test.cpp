// Codegen — §22.12 rule 1 (v0.228) / §2.8.3 rule 4, issue #1513: the
// per-INSTANTIATION lazy-init guard for a generic type's static state
// (genericInstEnsureInitFn, LlvmBackend_Setup.cpp) must be at atomic-CAS +
// cross-domain-blocking parity with the per-TYPE guard #1391 hardened
// (emitEnsureInitFns, pinned by static_init_atomic_1391_test.cpp).
//
// #1513's own fix made a generic instantiation's static-field global and its
// lazy-init guard LinkOnceODR — genuinely ONE shared copy process-wide across
// every consuming artifact, not a per-module-private one. That is exactly
// what makes a plain (non-atomic) load/store guard dangerous here: two
// DOMAINS racing to first-touch the SAME instantiation (in the SAME module,
// or across two SEPARATE importing modules whose LinkOnceODR copies the
// linker merges to one) could both observe uninit and both run the
// instantiation's sfinit — a reachable double-init, not a theoretical one,
// once the state is genuinely shared. This test pins that the guard was
// brought to the SAME mechanism #1391 uses for the per-type case: a strong
// `cmpxchg` for the uninit->in-progress claim, atomic acquire/release
// ordering on the state transitions, and the same-thread-vs-cross-domain
// dispatch (chksame/crosswait) that blocks a genuine cross-domain touch via
// bz_static_init_wait/notify (bezel_runtime_concurrency.cpp) instead of
// racing straight through.
//
// Mirrors static_init_atomic_1391_test.cpp's IR-inspection technique (the fix
// is a codegen change with no Bezel-level syntax to assert on) and its
// bezelcPath/compileAndCaptureIR helpers verbatim; only the slice marker and
// the source shape (a GENERIC class, and a second class whose method call
// forces monomorphization of one concrete instantiation) differ.

#include <unistd.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDirGenStaticAtomic1513() {
    return wtScratchDir("wt-genstaticatomic1513");
}

// See static_init_atomic_1391_test.cpp's identical helper for the full
// rationale (resolving bezelc relative to this test binary; subprocess
// capture of BEZEL_DUMP_IR's stderr dump rather than in-process Driver, which
// proved unreliable for capturing raw_fd_ostream output).
std::string bezelcPath() {
    std::array<char, 4096> buf{};
    ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n <= 0) return "";
    fs::path exe(std::string(buf.data(), static_cast<std::size_t>(n)));
    return (exe.parent_path().parent_path() / "bezelc").string();
}

std::string compileAndCaptureIR(const std::string& source) {
    std::string bezelc = bezelcPath();
    if (bezelc.empty() || !fs::exists(bezelc)) return "";
    auto dir = wtDirGenStaticAtomic1513();
    std::string stem = unique_stem("bezel_genstaticatomic1513");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string obj = (dir / (stem + ".o")).string();
    {
        std::ofstream out(src);
        out << source;
    }

    std::string cmd = "BEZEL_DUMP_IR=1 '" + bezelc + "' -c -o '" + obj +
                      "' '" + src + "' 2>&1";
    std::string text;
    if (FILE* p = popen(cmd.c_str(), "r")) {
        std::array<char, 8192> chunk{};
        std::size_t got;
        while ((got = fread(chunk.data(), 1, chunk.size(), p)) > 0) {
            text.append(chunk.data(), got);
        }
        pclose(p);
    }
    std::remove(src.c_str());
    std::remove(obj.c_str());
    return text;
}

// Slices out ONE ensureinit function's body from dumped module IR text, from
// its `define ... @"...<marker>"() {` line to the matching closing `}`.
//
// #1488 / #1513: `marker` must be a TAIL anchor — `<mangle-suffix>$ensureinit`
// — never a `bz$<Type>` PREFIX. #1488 module-qualifies every cross-module
// symbol (`bz$<module>.<Type>...`) and #1513's per-instantiation guard adds a
// SECOND qualifier on top of that (`bz$<module>.<Type>$<type-arg-mangle>$...`,
// e.g. `bz$M.Reg$int$ensureinit`) — a prefix-anchored marker breaks the moment
// either qualifier changes (this exact failure mode broke
// static_init_atomic_1391_test.cpp's own marker once already, per that file's
// comment). The trailing `"() {` distinguishes the DEFINITION from a CALL
// SITE (`call void @"...ensureinit"()`, emitted at every static-access site),
// whose `()` is followed by a newline, not ` {`.
//
// For the per-instantiation case this file also needs to tell the TEMPLATE-
// level ensureinit (`bz$M.Reg$ensureinit`, non-generic-instantiation, always
// present but a no-op/dead path for a generic type per emitEnsureInitFns'
// #796 comment) apart from the INSTANTIATION-level one this fix added
// (`bz$M.Reg$int$ensureinit`) — both contain "Reg" and "ensureinit". Anchoring
// on the instantiation mangle (`"int$ensureinit"...`) rather than just
// `"Reg$ensureinit"` picks the right one specifically.
std::string sliceEnsureInit(const std::string& ir, const std::string& marker) {
    std::string full = marker + "\"() {";
    auto start = ir.find(full);
    if (start == std::string::npos) return "";
    auto bodyStart = ir.find('{', start);
    if (bodyStart == std::string::npos) return "";
    auto bodyEnd = ir.find("\n}", bodyStart);
    if (bodyEnd == std::string::npos) return "";
    return ir.substr(bodyStart, bodyEnd - bodyStart);
}

// Shared source: a generic Reg<T> with a lazily-initialized (non-baked —
// Seed() is a call, forcing real sfinit/ensureinit machinery, not a
// compile-time-baked constant) static field, and a second, non-generic
// Trigger class whose method call forces monomorphization of the CONCRETE
// instantiation Reg<int> — the per-instantiation guard this fix built is
// created lazily, only once something actually touches a concrete
// instantiation.
const char* kSource =
    "module M; "
    "public class Reg<T> "
    "  public static readonly _v: int := Seed(); "
    "  private static method Seed(): int => 42; "
    "  public static method Get(): int => _v; "
    "end Reg; "
    "public class Trigger "
    "  public static method Touch(): int => Reg<int>.Get(); "
    "end Trigger;";

}  // namespace

// §22.12 rule 1 / §2.8.3 rule 4, #1513: the per-instantiation ensureinit's
// uninit/in-progress dispatch is a strong `cmpxchg ... i8 0, i8 1 ...
// acq_rel acquire` — same mechanism, same ordering, as the per-type guard's
// #1391 fix; NOT the plain load/store TOCTOU-racy sequence #1513's fix used
// on its own first draft (before the coordinator's review caught the gap and
// this test was added to pin the correction).
BEZEL_TEST(generic_static_atomic_1513, ensureinit_uses_atomic_cmpxchg_for_claim) {
    std::string ir = compileAndCaptureIR(kSource);
    BEZEL_EXPECT_TRUE(!ir.empty());
    std::string fn = sliceEnsureInit(ir, "int$ensureinit");
    BEZEL_EXPECT_TRUE(!fn.empty());
    BEZEL_EXPECT_TRUE(fn.find("cmpxchg") != std::string::npos);
    BEZEL_EXPECT_TRUE(fn.find("acq_rel") != std::string::npos);
}

// §22.12 rule 1, #1513: the DONE fast-path load is an atomic ACQUIRE load and
// both terminal transitions (setdone -> 2, setfail -> 3) are atomic RELEASE
// stores — the happens-before edge that guarantees a domain observing
// state==done also observes the completed instantiation's static-field
// writes, not a torn or reordered view. Exactly two release stores (one per
// terminal transition), mirroring static_init_atomic_1391_test.cpp's
// per-type Test 2.
BEZEL_TEST(generic_static_atomic_1513,
           ensureinit_uses_acquire_load_and_release_stores) {
    std::string ir = compileAndCaptureIR(kSource);
    BEZEL_EXPECT_TRUE(!ir.empty());
    std::string fn = sliceEnsureInit(ir, "int$ensureinit");
    BEZEL_EXPECT_TRUE(!fn.empty());
    BEZEL_EXPECT_TRUE(fn.find("load atomic i8") != std::string::npos);
    BEZEL_EXPECT_TRUE(fn.find("acquire") != std::string::npos);
    std::size_t pos = 0, releaseStores = 0;
    while ((pos = fn.find("store atomic i8", pos)) != std::string::npos) {
        auto lineEnd = fn.find('\n', pos);
        std::string line = fn.substr(pos, lineEnd - pos);
        if (line.find("release") != std::string::npos) ++releaseStores;
        pos = (lineEnd == std::string::npos) ? fn.size() : lineEnd;
    }
    BEZEL_EXPECT_EQ(static_cast<int>(releaseStores), 2);
}

// §22.12 rule 1's SECOND clause ("a domain touching a type mid-initialization
// by another domain blocks until initialization completes"), #1513: a
// losing, CROSS-thread CAS on the per-instantiation guard (old==1) must
// reach the `chksame`/`crosswait` blocks and call bz_static_init_wait
// (bezel_runtime_concurrency.cpp — the SAME process-wide runtime pair
// emitEnsureInitFns uses, reused unmodified: that pair is keyed by the state
// global's own ADDRESS, and this instantiation's LinkOnceODR state global has
// exactly one such address process-wide after linking, so no new runtime
// entry point was needed). Both terminal transitions must call
// bz_static_init_notify to wake any waiter — exactly two call sites,
// mirroring the two release stores above.
BEZEL_TEST(generic_static_atomic_1513,
           ensureinit_blocks_cross_thread_instead_of_proceeding) {
    std::string ir = compileAndCaptureIR(kSource);
    BEZEL_EXPECT_TRUE(!ir.empty());
    std::string fn = sliceEnsureInit(ir, "int$ensureinit");
    BEZEL_EXPECT_TRUE(!fn.empty());
    BEZEL_EXPECT_TRUE(fn.find("chksame:") != std::string::npos);
    BEZEL_EXPECT_TRUE(fn.find("crosswait:") != std::string::npos);
    BEZEL_EXPECT_TRUE(fn.find("call i8 @bz_static_init_wait") != std::string::npos ||
                       fn.find("call signext i8 @bz_static_init_wait") !=
                           std::string::npos);
    std::size_t pos = 0, notifyCalls = 0;
    while ((pos = fn.find("@bz_static_init_notify", pos)) != std::string::npos) {
        ++notifyCalls;
        pos += 1;
    }
    BEZEL_EXPECT_EQ(static_cast<int>(notifyCalls), 2);
}

// Sanity/negative check: the marker discipline itself. The TEMPLATE-level
// ensureinit (`bz$M.Reg$ensureinit`, always emitted for a type with static
// state per emitEnsureInitFns, but a dead/no-op path for a GENERIC type since
// nothing ever calls it — #796's own comment) must NOT be what
// sliceEnsureInit("int$ensureinit") finds; confirms the instantiation-mangle
// anchor picks the per-instantiation function specifically, not the
// template-level one both of which contain the substring "Reg" and
// "ensureinit".
BEZEL_TEST(generic_static_atomic_1513,
           marker_selects_instantiation_not_template_level_ensureinit) {
    std::string ir = compileAndCaptureIR(kSource);
    BEZEL_EXPECT_TRUE(!ir.empty());
    // The template-level ensureinit exists (dead code, but still emitted)...
    BEZEL_EXPECT_TRUE(ir.find("Reg$ensureinit\"() {") != std::string::npos);
    // ...and is InternalLinkage (per-module, never exported for a generic
    // type — emitEnsureInitFns' own "generics: cross-module deferred" gate),
    // unlike the LinkOnceODR per-instantiation one this fix added.
    std::string fn = sliceEnsureInit(ir, "int$ensureinit");
    BEZEL_EXPECT_TRUE(!fn.empty());
    BEZEL_EXPECT_TRUE(ir.find("define linkonce_odr void @\"bz$M.Reg$int$ensureinit\"") !=
                       std::string::npos);
}

// ---------------------------------------------------------------------------
// Regression: a generic class with a CONSTANT-LITERAL static field AND a
// NON-CONSTANT static field — the gate's canary caught this exact shape.
// ---------------------------------------------------------------------------
//
// Root cause (a genuine regression introduced by this fix's own first draft,
// caught by the integration gate's `codegen_generic_static_nonconst_init_709`
// / `codegen_generic_static_sameclass_715` canaries running alongside another
// producer's edits to the same file — both suites failed alone, 5/5 tries):
// genericInstSfinitFn discovered which fields to initialize by scanning
// genericInstStaticGlobals_ for entries ALREADY created for this
// instantiation — but that scan runs once, lazily, the FIRST time
// genericInstSfinitFn is built (triggered by the FIRST field of the
// instantiation any code touches), and the function is then CACHED. In
// `Mixed<int>.literal * 100 + Mixed<int>.computed`, `literal` is touched
// first (left-to-right evaluation): this lazily builds sfinit while
// genericInstStaticGlobals_ contains ONLY `literal`'s entry, so `literal`
// (10) is stored correctly. `computed`'s per-instantiation global is created
// moments later, by ITS OWN access — but genericInstSfinitFn was already
// finalized and cached, so `computed`'s initializer (Helper3.Base() * 2 = 6)
// is silently never stored, leaving the per-instantiation global at its
// zero-init default. `literal` reads correctly (10) while `computed` reads
// 0 (wrong; must be 6) — the exact "for a MIXED class the non-constant
// initializer never runs" shape.
//
// Fix: genericInstSfinitFn now discovers ALL of ts's static fields UPFRONT by
// iterating ts->members (not genericInstStaticGlobals_), calling
// getOrCreateGenericInstStaticGlobal for each — idempotent (a cache hit for a
// field some other access already redirected, a fresh creation otherwise) —
// so every field's per-instantiation global exists and gets processed
// regardless of which field's access happens to trigger the lazy build.
//
// This is the SAME source shape as the pre-existing
// generic_static_nonconst_init_709_test.cpp's
// mixed_constant_and_nonconstant_fields_spec_22_12_22_13 (which this fix's
// first draft broke and which re-passing pins the fix); this test additionally
// ties the exact repro handed back by review directly to #1513, using the
// review's own `literal * 100 + computed` shape (so the expected value
// distinguishes "computed silently stayed 0" — 1000 — from "computed correctly
// became 6" — 1006 — by more than a coincidental match).
BEZEL_TEST(generic_static_atomic_1513,
           mixed_constant_and_nonconstant_static_fields_regression_spec_22_12) {
    const std::string prog =
        "module M; "
        "class Helper3 "
        "  public static method Base(): int return 3; end Base; "
        "end Helper3; "
        "class Mixed<T> "
        "  public static literal: int := 10; "
        "  public static computed: int := Helper3.Base() * 2; "
        "end Mixed; "
        "class Program "
        "  public static method Main(): int "
        "    return Mixed<int>.literal * 100 + Mixed<int>.computed; "
        "  end Main; "
        "end Program;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    // 10 * 100 + 6 = 1006, masked to 8 bits by the process exit status
    // (WEXITSTATUS, see aot_support.hpp's own note on exitCode) — 1006 % 256
    // = 238. Pre-fix: 10 * 100 + 0 = 1000 (computed silently never
    // initialized), masked = 1000 % 256 = 232 — the exact exit code the
    // review's own manual `build/bezelc` repro observed before this fix.
    BEZEL_EXPECT_EQ(r.exitCode, 238);
}

// Same shape, SECOND instantiation (Mixed<string>) — confirms the fix isn't
// accidentally specific to whichever instantiation happens to be built first
// in the process, and that literal/computed remain correctly isolated
// per-instantiation (§22.12) alongside being correctly INITIALIZED
// per-instantiation (this fix).
BEZEL_TEST(generic_static_atomic_1513,
           mixed_constant_and_nonconstant_static_fields_second_instantiation) {
    const std::string prog =
        "module M; "
        "class Helper3 "
        "  public static method Base(): int return 3; end Base; "
        "end Helper3; "
        "class Mixed<T> "
        "  public static literal: int := 10; "
        "  public static computed: int := Helper3.Base() * 2; "
        "end Mixed; "
        "class Program "
        "  public static method Main(): int "
        "    var a := Mixed<int>.literal * 100 + Mixed<int>.computed; "
        "    var b := Mixed<string>.literal * 100 + Mixed<string>.computed; "
        "    if a = 1006 and b = 1006 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// Reversed touch order — computed accessed BEFORE literal. Pins that the fix
// is order-independent (iterates ts->members, not "whichever field was
// touched first"), not merely "the first field of the expression happens to
// initialize correctly."
BEZEL_TEST(generic_static_atomic_1513,
           mixed_fields_reversed_touch_order_still_correct) {
    const std::string prog =
        "module M; "
        "class Helper3 "
        "  public static method Base(): int return 3; end Base; "
        "end Helper3; "
        "class Mixed<T> "
        "  public static literal: int := 10; "
        "  public static computed: int := Helper3.Base() * 2; "
        "end Mixed; "
        "class Program "
        "  public static method Main(): int "
        "    return Mixed<int>.computed * 100 + Mixed<int>.literal; "
        "  end Main; "
        "end Program;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    // 6 * 100 + 10 = 610, masked to 8 bits (WEXITSTATUS) = 610 % 256 = 98.
    // Pre-fix, the bug is symmetric in touch order, not tied to "literal"
    // specifically: whichever field is accessed FIRST gets included in the
    // lazily-built sfinit's snapshot and initializes correctly; whichever is
    // accessed SECOND is created too late and stays at its zero-init
    // default. Here `computed` is first, so a pre-fix run would print
    // `computed`=6 (correct) and `literal`=0 (wrong, its zero-init default
    // rather than the literal 10) — 6*100+0 = 600, masked = 88, not 98. This
    // pins that the fix discovers every field regardless of which one a
    // given expression happens to touch first, not just "whichever field the
    // #709 test's own expression order favors."
    BEZEL_EXPECT_EQ(r.exitCode, 98);
}

// fix #2948 — §22.13 (v0.260): generic statics initialize once, in textual
// declaration order. First reads the later mutable Second while Second still
// holds its §7.9 default, so First must become 107. The initializer deliberately
// uses `90 + "abc".Length` rather than a literal, distinguishing this from a
// constant-folder bug. Before the fix, lowering that sibling read recursively
// built a second per-instantiation ensureinit guard; the second guard reran
// sfinit after Second became 93 and silently overwrote First with 200.
BEZEL_TEST(generic_static_atomic_1513,
           later_mutable_static_read_uses_textual_order_default_2948) {
    const std::string prog =
        "module M; "
        "class Og<T> "
        "  public v: T; "
        "  public static readonly First: int := 107 + Second; "
        "  public static Second: int := 90 + \"abc\".Length; "
        "end Og; "
        "class Program "
        "  public static method Main(): int "
        "    return Og<string>.First; "
        "  end Main; "
        "end Program;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 107);
}
