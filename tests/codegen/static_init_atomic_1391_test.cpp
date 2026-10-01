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
// Tests 1-2 are a mechanism check: they inspect the LLVM IR the compiler
// emits (via BEZEL_DUMP_IR, LlvmBackend.cpp) for the per-type init-state
// global and assert the atomic cmpxchg / atomic load-acquire / atomic
// store-release instructions are present — the fix is a codegen change with
// no Bezel-level syntax to assert on, so IR inspection is the direct way to
// pin it. Reverting the fix (plain load/store, unconditional store-then-init)
// makes this assertion fail; see the #1391 hand-off note for the manual
// before/after verification performed when the fix landed (revert via
// `git checkout`, rebuild, re-probe — never `git stash`, per this repo's
// worktree discipline).
//
// The SECOND half of #1391 — a domain that loses the CAS with old==1
// (another domain's init genuinely in progress) BLOCKING until that other
// domain's initialization completes or fails, rather than proceeding
// immediately (rule 1's second clause) — is fixed below the CAS half in this
// same commit sequence. Tests 3-6 cover it:
//   - Test 3 is a mechanism check (same IR-inspection technique as 1-2) for
//     the new `chksame`/`crosswait` blocks and the bz_static_init_wait /
//     bz_static_init_notify runtime calls (bezel_runtime_concurrency.cpp).
//   - Test 4 extends the Test-6-style stress harness below with a
//     deliberately SLOW initializer (a busy loop) to widen the in-progress
//     window, and asserts every concurrently-touching domain observes the
//     initializer's FINAL value, never a not-yet-assigned default — the
//     direct, observable consequence of blocking instead of proceeding. Per
//     this task's protocol: this is a best-effort, timing-dependent stress
//     test, not a proof the race is structurally impossible (unlike Test 3,
//     which pins the mechanism unconditionally); its comment states the
//     calibration and the honest limits.
//   - Test 5 is the adversarial "initializer throws while other domains are
//     BLOCKED on it" scenario (§22.7's failed-state machinery): every blocked
//     waiter must wake and reraise TypeInitializationException, not hang
//     forever. Same best-effort timing caveat as Test 4.
//   - Test 6 (originally Test 2) is the pre-existing double-init stress test,
//     unchanged.

#include <unistd.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

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
    std::string bezelc = bezelcPath();
    if (bezelc.empty() || !fs::exists(bezelc)) return "";
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_staticatomic1391");
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
    // The `bz$` prefix is deliberately NOT part of the marker: #1488 qualified
    // every cross-module symbol by its declaring module, so the definition is
    // now `@"bz$<module>.<Type>$ensureinit"() {` (e.g. `bz$M.Counter$...`) and
    // a marker anchored at `bz$<Type>` stopped matching — `fn` came back empty
    // and every assertion below failed at once, which is what a gate caught.
    // Matching on the `<Type>$ensureinit"() {` tail is robust to the module
    // qualifier while still being specific to this one function, and still
    // distinguishes the DEFINITION from a call site via the trailing "() {".
    std::string marker = typeName + "$ensureinit\"() {";
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

// §22.12 rule 1 (v0.228) mechanism check — the BLOCKING half of #1391: a
// losing, CROSS-thread CAS (old==1) must call bz_static_init_wait
// (bezel_runtime_concurrency.cpp) instead of falling straight through to
// `ret`, and both terminal transitions (setdone/setfail) must call
// bz_static_init_notify to wake any thread parked there. Distinguishing
// same-thread re-entrancy (proceed) from a cross-domain touch (block) needs
// a same-thread comparison against the owner-thread global — the `chksame`
// block — before ever reaching the `crosswait` block that calls
// bz_static_init_wait. Reverting just this half (restoring `chkOldFailed`'s
// unconditional `oldFailed ? failedB : retB` and dropping the two notify
// calls, i.e. exactly what b122241 left as #1391's open remainder) makes
// this assertion fail while Tests 1-2 above still pass — this test is scoped
// to the incremental piece this session adds.
BEZEL_TEST(static_init_atomic_1391, ensureinit_blocks_cross_thread_instead_of_proceeding) {
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
    // The same-thread-vs-cross-thread dispatch block and the blocking block.
    BEZEL_EXPECT_TRUE(fn.find("chksame:") != std::string::npos);
    BEZEL_EXPECT_TRUE(fn.find("crosswait:") != std::string::npos);
    // The blocking wait call, taking the state global's address and
    // returning the terminal state (i8).
    BEZEL_EXPECT_TRUE(fn.find("call i8 @bz_static_init_wait") != std::string::npos ||
                       fn.find("call signext i8 @bz_static_init_wait") !=
                           std::string::npos);
    // Both terminal transitions (setdone, setfail) notify waiters — expect
    // exactly two call sites, matching the two release stores Test 2 checks.
    std::size_t pos = 0, notifyCalls = 0;
    while ((pos = fn.find("@bz_static_init_notify", pos)) != std::string::npos) {
        ++notifyCalls;
        pos += 1;
    }
    BEZEL_EXPECT_EQ(static_cast<int>(notifyCalls), 2);
}

// §22.12 rule 1 (v0.228) behavioral stress test — the BLOCKING half of
// #1391.
//
// `Slow2`'s static initializer runs a long, deliberately CPU-bound busy loop
// BEFORE its `readonly` field's sfinit store lands, widening the in-progress
// window enough that several domains spawned immediately after the first
// touch reliably land INSIDE it (see the calibration note below). Each
// spawned domain reads `Slow2.Get()` and records via an already-warmed
// AtomicInteger<int> pair (Recorder2, settled single-threaded before any
// spawn, exactly as Recorder is in the stress test below) whether it
// observed the initializer's FINAL value (999) or anything else.
//
// With the fix (blocking): every domain that loses the CAS BLOCKS until the
// winner's initializer completes, so every read observes exactly 999 —
// WrongCount() must be 0, deterministically: this is the direct, structural
// consequence of blocking (a blocked domain's ensureinit call cannot return
// until state is done/failed), not a timing coincidence.
//
// Without the fix (the b122241-only baseline this session started from): a
// losing domain proceeds immediately past ensureinit and reads whatever
// value the field currently holds — its DEFAULT (0, an int field) until
// sfinit's store lands at the very end of the busy loop. With the loop
// calibrated wide enough this reliably (though, per this task's protocol,
// not by formal proof — stated plainly) reproduces WrongCount() > 0; a
// manual revert-rebuild-run cycle against the pre-blocking-fix source (same
// scratch-copy protocol as the #1391 hand-off note, never `git stash`)
// confirmed this before the busy-loop iteration count below was finalized.
//
// Calibration: BUSY_ITERS's magnitude targets roughly a few hundred
// milliseconds of wall time on the reference machine (verified via a probe
// build with `Stopwatch`, stdlib §8.1, before this constant was chosen) —
// comfortably wider than the microsecond-scale gap between spawning four
// domains back-to-back and the pool dispatching each one's `Touch2`
// continuation. This is a best-effort, timing-dependent test (like the
// double-init stress test below): it does not prove the race is
// structurally impossible, only that it does not reproduce in practice with
// a wide window. Test 3 above is the unconditional, timing-independent pin
// for this half of the fix.
BEZEL_TEST(static_init_atomic_1391, cross_domain_touch_blocks_until_init_completes) {
    const std::string source =
        "module M; "
        "import System.Threading; "
        "class Recorder2 "
        "  private static _correct: AtomicInteger<int> := new AtomicInteger<int>(0); "
        "  private static _wrong: AtomicInteger<int> := new AtomicInteger<int>(0); "
        "  public static method RecordCorrect(): int => _correct.Increment(); "
        "  public static method RecordWrong(): int => _wrong.Increment(); "
        "  public static method CorrectCount(): int => _correct.Value; "
        "  public static method WrongCount(): int => _wrong.Value; "
        "end Recorder2; "
        "class Slow2 "
        "  public static readonly _v: int := ComputeSlow(); "
        "  private static method ComputeSlow(): int "
        "    var acc: int := 0; "
        "    var i: int := 0; "
        "    while i < 400000000 do acc := acc + 1; i := i + 1; end; "
        "    return 999; "
        "  end ComputeSlow; "
        "  public static method Get(): int => _v; "
        "end Slow2; "
        "class Toucher2 "
        "  public static method Touch2() "
        "    var v: int := Slow2.Get(); "
        "    if v = 999 then _ := Recorder2.RecordCorrect(); "
        "    else _ := Recorder2.RecordWrong(); end; "
        "  end Touch2; "
        "end Toucher2; "
        "class Program2 "
        "  public static async method Main(): Task<int> "
        "    _ := Recorder2.CorrectCount(); "  // settle Recorder2's OWN lazy init first
        "    using var t1 := Domain.Spawn(Toucher2.Touch2); "
        "    using var t2 := Domain.Spawn(Toucher2.Touch2); "
        "    using var t3 := Domain.Spawn(Toucher2.Touch2); "
        "    using var t4 := Domain.Spawn(Toucher2.Touch2); "
        "    await t1; "
        "    await t2; "
        "    await t3; "
        "    await t4; "
        "    return Recorder2.WrongCount(); "
        "  end Main; "
        "end Program2;";
    auto r = bezel::test::runAot(source);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);  // WrongCount() must be exactly 0
}

// §22.12 rule 1 (v0.228) / §22.7 adversarial stress test — an initializer
// that THROWS while OTHER domains are blocked on it (#1391).
//
// `Thrower`'s static initializer busy-loops (same widening technique and
// calibration as the previous test) and then throws. The CAS winner runs it,
// eventually reaches failB — a RELEASE store of state=3(failed) followed by
// bz_static_init_notify (LlvmBackend_Setup.cpp) — and the outer `ensureinit`
// call reraises for the winner itself via the normal §22.7 path. Every OTHER
// domain that touched `Thrower` while the winner's busy loop was still
// running is, by this fix, parked in bz_static_init_wait (crosswait); this
// test's central question is whether failB's notify reliably wakes them
// (never leaving a blocked domain waiting forever) and whether the woken
// `waited == 3` branch correctly routes into failedB (RE-RAISE, §22.7 —
// "the ordinary failed-state machinery caches it"), rather than, say,
// silently proceeding as if initialization had succeeded.
//
// Each spawned domain wraps its touch in try/catch and records via
// Recorder3 (an already-warmed AtomicInteger<int> pair, settled
// single-threaded before any spawn, exactly as Recorder/Recorder2 above)
// whether it caught TypeInitializationException. With the fix: every domain
// — the winner (reraising its own failure) and every blocked loser (waking
// into the failed state) alike — must catch exactly one
// TypeInitializationException each; UncaughtCount() must stay 0 and
// CaughtCount() must equal the domain count. A domain that hung instead
// would make this test TIME OUT rather than fail an assertion — the harness
// itself is the deadlock detector here.
//
// Same honesty caveat as the previous test: this is a best-effort,
// timing-dependent stress test for the *value/liveness* consequence of the
// notify call, not a formal proof; Test 3 is the unconditional mechanism pin
// for the notify calls' existence.
BEZEL_TEST(static_init_atomic_1391, blocked_waiters_wake_into_failed_state_reraise) {
    const std::string source =
        "module M; "
        "import System.Threading; "
        "class Recorder3 "
        "  private static _caught: AtomicInteger<int> := new AtomicInteger<int>(0); "
        "  private static _uncaught: AtomicInteger<int> := new AtomicInteger<int>(0); "
        "  public static method RecordCaught(): int => _caught.Increment(); "
        "  public static method RecordUncaught(): int => _uncaught.Increment(); "
        "  public static method CaughtCount(): int => _caught.Value; "
        "  public static method UncaughtCount(): int => _uncaught.Value; "
        "end Recorder3; "
        "class Thrower "
        "  public static readonly _v: int := Boom(); "
        "  private static method Boom(): int "
        "    var acc: int := 0; "
        "    var i: int := 0; "
        "    while i < 400000000 do acc := acc + 1; i := i + 1; end; "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Boom; "
        "  public static method Get(): int => _v; "
        "end Thrower; "
        "class Toucher3 "
        "  public static method Touch3() "
        "    try "
        "      _ := Thrower.Get(); "
        "      _ := Recorder3.RecordUncaught(); "  // must never be reached
        "    catch TypeInitializationException => _ := Recorder3.RecordCaught(); "
        "    end; "
        "  end Touch3; "
        "end Toucher3; "
        "class Program3 "
        "  public static async method Main(): Task<int> "
        "    _ := Recorder3.CaughtCount(); "  // settle Recorder3's OWN lazy init first
        "    using var t1 := Domain.Spawn(Toucher3.Touch3); "
        "    using var t2 := Domain.Spawn(Toucher3.Touch3); "
        "    using var t3 := Domain.Spawn(Toucher3.Touch3); "
        "    using var t4 := Domain.Spawn(Toucher3.Touch3); "
        "    await t1; "
        "    await t2; "
        "    await t3; "
        "    await t4; "
        "    if Recorder3.UncaughtCount() = 0 then return Recorder3.CaughtCount(); end; "
        "    return -1; "
        "  end Main; "
        "end Program3;";
    auto r = bezel::test::runAot(source);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);  // all 4 domains caught exactly one TIE each
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
