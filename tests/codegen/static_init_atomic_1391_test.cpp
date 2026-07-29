// Codegen — §22.12 rule 1 (v0.228): "a type's static initializer runs exactly
// once process-wide; a domain touching a type mid-initialization by another
// domain blocks until initialization completes (or observes the failed
// state, §22.7); the runtime records the single global initialization order"
// (which is what makes §22.13's reverse-order exit disposal well-defined).
//
// Issue #1391: before this fix, `bz$<T>$ensureinit` (LlvmBackend_Setup.cpp,
// emitEnsureInitFns) guarded the per-type init-state byte with a PLAIN,
// non-atomic `load i8` / `store i8` and an unconditional "if state==0, store 1
// and run the initializer" sequence — a check-then-act race: two domains could
// both observe state==0 and both fall through to running the type's static
// initializer (double-init; per §22.13 each recording its own field stores and
// disposal-order entry, i.e. double-dispose at exit too), and no memory-model
// guarantee tied a completed initializer's field writes to a later domain's
// observation of state==done.
//
// This fix (the "smaller, self-contained half" of #1391 — see the hand-off
// note in the issue) makes the 0(uninit)->1(in-progress) transition a single
// atomic compare-exchange (strong, AcquireRelease-on-success /
// Acquire-on-failure), and the terminal 1->2(done) / 1->3(failed) transitions
// atomic RELEASE stores paired with an atomic ACQUIRE load/CAS-failure-read on
// the observing side. At most one domain's CAS can ever observe old==0 and
// become the initializer; every other domain's CAS fails and is handed the
// CAS's own freshly-observed value (never the outer, possibly-stale fast-path
// load), so it can never mistake an in-progress or already-failed type for
// uninitialized.
//
// What this fix does NOT yet do (tracked as #1391's remaining half, and named
// explicitly in the emitEnsureInitFns comments this test's Test 1 inspects):
// a domain that loses the CAS with old==1 (another domain's init genuinely
// in progress) still falls through and PROCEEDS immediately rather than
// BLOCKING until that other domain's initialization completes, as rule 1's
// second clause requires. Test 2 below is therefore a best-effort STRESS test
// for the half that *is* fixed (no double-init), not a proof that a losing
// domain never observes a partially-initialized static through some other
// path — see its comment for the precise claim and its limits.
//
// Test 1 is a mechanism check: it inspects the LLVM IR the compiler emits
// (via BEZEL_DUMP_IR, LlvmBackend.cpp) for the per-type init-state global and
// asserts the atomic cmpxchg / atomic load-acquire / atomic store-release
// instructions are present — the fix is a codegen change with no Bezel-level
// syntax to assert on, so IR inspection is the direct way to pin it. Reverting
// the fix (plain load/store, unconditional store-then-init) makes this
// assertion fail; see the #1391 hand-off note for the manual before/after
// verification performed when the fix landed (revert via `git checkout`,
// rebuild, re-probe — never `git stash`, per this repo's worktree discipline).

#include <unistd.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "test_harness.hpp"

namespace {

namespace fs = std::filesystem;

// The `bezelc` binary sits alongside this test binary's build tree:
// `<build>/tests/bezel_tests_codegen` (this process) and `<build>/bezelc`
// (its sibling under the SAME build dir, one level up). Resolved via
// /proc/self/exe rather than a hardcoded path so this works from any
// worktree's own build directory.
std::string bezelcPath() {
    std::array<char, 4096> buf{};
    ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n <= 0) return "";
    fs::path exe(std::string(buf.data(), static_cast<std::size_t>(n)));
    return (exe.parent_path().parent_path() / "bezelc").string();
}

// Compiles `source` to an object (no link, no Main required) with
// BEZEL_DUMP_IR=1 set, capturing the LLVM module text the compiler prints to
// stderr (LlvmBackend.cpp: `if (... getenv("BEZEL_DUMP_IR")) m->print(errs()...)`).
// Runs the real `bezelc` binary as a SUBPROCESS (via popen) rather than
// calling Driver in-process: LLVM's `errs()` is a raw_fd_ostream, and
// capturing it by redirecting this test process's own fd 2 around the call
// proved unreliable (buffering/timing not fully in this test's control,
// confirmed empirically — see the #1391 hand-off note); a subprocess's
// stdout/stderr is captured to completion by definition once the pipe closes
// (the process has exited), so there is no such race. Returns "" on any
// failure to locate/run bezelc (never mistaken for "no atomic ops emitted" by
// the assertions below, which require both non-empty AND the markers).
std::string compileAndCaptureIR(const std::string& source) {
    static int counter = 0;
    std::string bezelc = bezelcPath();
    if (bezelc.empty() || !fs::exists(bezelc)) return "";
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_staticatomic1391_" + std::to_string(counter++);
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

// Slices out the `bz$<Type>$ensureinit` function body from dumped module IR
// text (from its `define internal void @"bz$<Type>$ensureinit"() {` line to
// the matching closing `}`), so the assertions below are scoped to exactly
// the function this fix changed rather than incidentally matching an atomic
// op emitted elsewhere in the module for an unrelated reason.
std::string sliceEnsureInit(const std::string& ir, const std::string& typeName) {
    // NOTE: must match the DEFINITION (`define ... @"bz$T$ensureinit"() {`),
    // not a CALL SITE (`call void @"bz$T$ensureinit"()`, emitted earlier in
    // the module at every static-access site, per emitEnsureStaticInit) — a
    // call site's `()` is followed by a newline, a definition's by ` {`. The
    // marker's trailing "() {" disambiguates; searching for just `...()`
    // matches the FIRST call site instead and silently slices the calling
    // function's body (found the hard way: #1391 test debugging).
    std::string marker = "@\"bz$" + typeName + "$ensureinit\"() {";
    auto start = ir.find(marker);
    if (start == std::string::npos) return "";
    auto bodyStart = ir.find('{', start);
    if (bodyStart == std::string::npos) return "";
    auto bodyEnd = ir.find("\n}", bodyStart);
    if (bodyEnd == std::string::npos) return "";
    return ir.substr(bodyStart, bodyEnd - bodyStart);
}

}  // namespace

// §22.12 rule 1 (v0.228) mechanism check: the CAS-based claim of the
// uninit->in-progress transition. A plain lazy static (readonly int, textual
// non-constant initializer — forces the field into `sfinit`, i.e. real
// runtime lazy-init machinery, not a compile-time-baked constant) must lower
// its ensureinit's uninit/in-progress dispatch to a strong `cmpxchg ... i8 0,
// i8 1 ... acq_rel acquire` — the fix that closes the double-init TOCTOU race
// (#1391).
BEZEL_TEST(static_init_atomic_1391, ensureinit_uses_atomic_cmpxchg_for_claim) {
    std::string ir = compileAndCaptureIR(
        "module M; "
        "class Counter "
        "  public static readonly _v: int := Seed(); "
        "  private static method Seed(): int => 42; "
        "  public static method Get(): int => _v; "
        "end Counter;");
    BEZEL_EXPECT_TRUE(!ir.empty());
    std::string fn = sliceEnsureInit(ir, "Counter");
    BEZEL_EXPECT_TRUE(!fn.empty());
    BEZEL_EXPECT_TRUE(fn.find("cmpxchg") != std::string::npos);
    BEZEL_EXPECT_TRUE(fn.find("acq_rel") != std::string::npos);
}

// §22.12 rule 1 (v0.228) mechanism check: the DONE fast-path load and the
// terminal state stores carry the acquire/release pairing that gives a
// later-touching domain a happens-before edge to the completed
// initializer's field writes (not just "no torn byte", which a plain aligned
// i8 store already gave for free on every real target — the atomic
// annotation is what the C++/LLVM memory model requires for the *ordering*
// promise rule 1 makes, independent of any one target's incidental
// behavior).
BEZEL_TEST(static_init_atomic_1391, ensureinit_uses_acquire_load_and_release_stores) {
    std::string ir = compileAndCaptureIR(
        "module M; "
        "class Counter "
        "  public static readonly _v: int := Seed(); "
        "  private static method Seed(): int => 42; "
        "  public static method Get(): int => _v; "
        "end Counter;");
    BEZEL_EXPECT_TRUE(!ir.empty());
    std::string fn = sliceEnsureInit(ir, "Counter");
    BEZEL_EXPECT_TRUE(!fn.empty());
    // Entry fast-path: `load atomic i8, ptr @"bz$Counter$initstate" acquire`.
    BEZEL_EXPECT_TRUE(fn.find("load atomic i8") != std::string::npos);
    BEZEL_EXPECT_TRUE(fn.find("acquire") != std::string::npos);
    // Both terminal transitions (setdone -> 2, setfail -> 3) are release
    // stores; expect exactly two `store atomic i8 ... release` sites.
    std::size_t pos = 0, releaseStores = 0;
    while ((pos = fn.find("store atomic i8", pos)) != std::string::npos) {
        auto lineEnd = fn.find('\n', pos);
        std::string line = fn.substr(pos, lineEnd - pos);
        if (line.find("release") != std::string::npos) ++releaseStores;
        pos = (lineEnd == std::string::npos) ? fn.size() : lineEnd;
    }
    BEZEL_EXPECT_EQ(static_cast<int>(releaseStores), 2);
}

// §22.12 rule 1 (v0.228) stress test — the double-init half of #1391.
//
// Many domains touch the SAME lazily-initialized type's static member for
// the first time with no ordering between them (Domain.Spawn gives no
// synchronization other than the join). `Slow`'s static initializer records
// every run of itself into a SEPARATE, already-warmed-up counter
// (`Recorder`, a §22.12 rule 2 form (i) admitted mutable static of
// `AtomicInteger<int>` — internally synchronized, so the count itself can
// never be lost to a torn/racy increment regardless of this fix). `Recorder`
// is deliberately touched once, single-threaded, BEFORE any domain is
// spawned, so ITS OWN lazy init is settled — Main is single-threaded up to
// the first `Domain.Spawn` call, so this warm-up is race-free — and the
// stress below exercises only `Slow`'s init, not a compound race on both
// types at once.
//
// With the fix: the CAS makes the uninit->in-progress transition exclusive,
// so exactly one domain ever runs `Slow`'s initializer — Recorder.Count()
// after every domain joins must be exactly 1, DETERMINISTICALLY (mutual
// exclusion does not depend on how the runtime happens to schedule the
// domains this run).
//
// Without the fix: the old plain-load/unconditional-store sequence has a
// narrow TOCTOU window (a handful of instructions between the load and the
// store), so this is NOT guaranteed to observe a double-init on every run —
// per this task's protocol, that is stated here rather than claimed away (an
// interactive repeated-run check against the reverted source did not manage
// to observe Recorder.Count() > 1 in a handful of tries — the window really
// is that narrow). The mechanical, revert-and-rebuild verification for #1391
// (see the issue hand-off note) is Tests 1/2 above, which DO fail outright
// without the fix, since `cmpxchg`/`acquire`/`release` never appear in the
// unfixed IR; this test is included anyway as the "did the fix actually
// change runtime behavior" sanity companion, with its limits stated plainly.
//
// The spawn entry is deliberately the zero-argument, no-result
// `Domain.Spawn(entry: () -> ())` overload (`Toucher.Touch()`, discarding
// `Slow.Get()`'s result internally) rather than the generic value-arg/
// value-result `Spawn<A, R>(entry: (A) -> R, arg: A): owned Task<R>`
// overload used elsewhere in this file's Test-authoring exploration: TWO OR
// MORE concurrent `Domain.Spawn` calls through that generic overload were
// found, independently of this fix (reproduces identically against the
// pre-#1391-fix compiler), to hang indefinitely at the `await` join — filed
// separately as #2333 rather than folded into this fix or worked around
// silently. The zero-arg/no-result overload with two OR MORE concurrent
// spawns (as exercised by the tsan_704_bz_exc_threadlocal_test.cpp precedent
// this shape is modeled on) does not exhibit that hang.
BEZEL_TEST(static_init_atomic_1391, concurrent_domains_run_lazy_static_init_exactly_once) {
    const std::string source =
        "module M; "
        "import System.Threading; "
        "class Recorder "
        "  private static _runs: AtomicInteger<int> := new AtomicInteger<int>(0); "
        "  public static method Bump(): int => _runs.Increment(); "
        "  public static method Count(): int => _runs.Value; "
        "end Recorder; "
        "class Slow "
        "  public static readonly _v: int := SlowInit(); "
        "  private static method SlowInit(): int "
        "    return Recorder.Bump(); "
        "  end SlowInit; "
        "  public static method Get(): int => _v; "
        "end Slow; "
        "class Toucher "
        "  public static method Touch() "
        "    _ := Slow.Get(); "
        "  end Touch; "
        "end Toucher; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    _ := Recorder.Count(); "  // settle Recorder's OWN lazy init first
        "    using var t1 := Domain.Spawn(Toucher.Touch); "
        "    using var t2 := Domain.Spawn(Toucher.Touch); "
        "    using var t3 := Domain.Spawn(Toucher.Touch); "
        "    using var t4 := Domain.Spawn(Toucher.Touch); "
        "    await t1; "
        "    await t2; "
        "    await t3; "
        "    await t4; "
        "    return Recorder.Count(); "
        "  end Main; "
        "end Program;";
    auto r = bezel::test::runAot(source);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}
