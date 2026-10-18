// Ownership / codegen — explicit `.Dispose()` of a MOVE-INITIALISED owned
// class LOCAL ran the destructor but never reclaimed the heap block. Every
// iteration of a loop leaked one object. Issue #3968.
//
// SPEC — settled, not underspecified. §28.3 "Scope-end and unwind disposal
// of owned class/interface locals" (v0.295), as cited by the explicit-Dispose
// reclamation clause in §28.11: the OBJECT whose disposal is invoked by an
// explicit `.Dispose()` is owned by the receiver's slot, and the explicit
// call site is responsible for both dispatching the destructor AND
// reclaiming the heap block when no other scope-end / unwind site covers the
// same slot (the §28.2 "exactly-once" rule is what makes a single free on
// this arm correct — the run-once flag makes a second DISPATCH a no-op,
// but a second FREE is a double-free, so only the FIRST disposal frees and
// the slot is nulled so any following auto-dispose reloads null and skips).
//
// WHY IT WAS MISSED. The explicit-dispose reclamation gate is
// `explicitDisposeOwnedReceiverSlot` (LlvmBackend_Calls2.cpp). It returns
// the slot address ONLY for shapes whose scope-end obligation is THIS
// scope's sole reclaim path — `using var`, owned-class init `new`/`call`,
// the `out owned` recipient, the owning-iterator variable, the
// `move coll[i]` slot-take shape, etc. (a-move, #2119). A plain
// `var y := move x;` was none of those: `unwindOwnedLocalClass(ld)` for `y`
// walked every arm (NewExpr / CallExpr / BinaryExpr / ConditionalExpr /
// CaseExpr / BindExpr / the move-IndexExpr slot-take) and FELL THROUGH —
// the move-IDENTIFIER shape was simply not classified.
// `explicitDisposeOwnedReceiverSlot` returned null, so the
// `else if (isFreshOwnedClassProducer(ma->target))` arm ran (also false —
// `y` is an IdentifierExpr, not a NewExpr or call) and the code landed on
// the bare `emitDynamicDispose(recv)` path: the destructor dispatched, the
// heap block was never freed. The scope-end auto-drop was unaffected
// (keyed on the TYPE through `registerUsingDisposal`, where `y` IS the
// owner), which is precisely why the leak surfaced only on the
// explicit-Dispose arm and stayed invisible to one-shot LSan.
//
// MEASURED PRE-FIX (parent 6460ea7 = origin/main, bezelc built clean):
//
//   var x := new Res(5); var y := move x; y.Dispose();   // 500 iterations
//     --sanitize=address, ASAN_OPTIONS=detect_leaks=1
//     -> "Direct leak of 7984 byte(s) in 499 object(s)"  byte-identical to
//        the issue's measurement; both compilers (`8d9d9e0` before #3950
//        and `9a69be0` after #3950) report the same counts.
//
// THE FIX. `unwindOwnedLocalClass` (LlvmBackend_Setup.cpp) recognizes a
// plain `move <identifier>` arm alongside the existing `move <index>` arm,
// classifying `y` as a sole-owner of a class/interface value when its
// initializer is `move <id>` and its declared type is a class/interface.
// The returned symbol drives `disposerFns_` lookup in `emitDisposals`, and
// the explicit-Dispose reclamation chain (which uses
// `unwindOwnedLocalClass` only indirectly, through
// `explicitDisposeOwnedReceiverSlot`) now ALSO returns the slot for `y` —
// so the explicit-Dispose arm reclaims + nulls and the auto-drop arm
// (already working) sees null on the reload.
//
// §28.8.1 ("a `move` expression... transfers the obligation to the new
// owner; the source becomes moved-from"), §28.11 (explicit Dispose reclaim
// clause), §28.3 (scope-end / unwind disposal of owned locals). v0.295,
// #3968.

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"       // runAot / runAotChecked — the AOT oracle
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// `class Res` is the disposable-shaped class from the issue (a destructor
// makes the destructor visible; `tag` is just so the program has something
// to return). `body` is the Main method body. The destructor also prints
// "D" so we can count disposals from stdoutText.
const char* kPrelude =
    "module M; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  destructor Console.WriteLine(\"D\"); end destructor; "
    "end Res; "
    "class C "
    "  public static method Main(): int ";

const char* kEpilogue =
    "  end Main; "
    "end C; ";

std::string wrap(const std::string& body) {
    return std::string(kPrelude) + body + kEpilogue;
}

// Count the number of times `needle` occurs in `hay` — a simple substring
// counter used to assert destructor counts from stdoutText.
int countOccurrences(const std::string& hay, const std::string& needle) {
    int count = 0;
    size_t pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        count++;
        pos += needle.size();
    }
    return count;
}

}  // namespace

// ============================================================================
// THE PRIMARY REGRESSION — the issue's exact reproducer in a 100-iter loop
// (a small enough count to keep the test fast, large enough to surface the
// leak under `detect_leaks=1`; the byte-identical `7984 B / 499 alloc`
// measured by the issue was at 500 iters, and 100 leaks ~16% of that).
//
// `runAotChecked` runs the generated executable under ASan/LSan so a heap
// block leak — what this issue's leak is — is caught by LSan (pre-fix:
// `jr.ran == false` because the LSan summary text matches the harness's
// "crash or sanitizer" gate). A SKIP guard is paired with this so a lane
// without the ASan toolchain does not pass vacuously; the gate tree
// always has the toolchain and so will catch the leak directly.
// ============================================================================

BEZEL_TEST(explicit_dispose_move_init_owned_local_leak_3968,
           issue_repro_move_init_local_explicit_dispose_28_3) {
    JitResult jr = runAotChecked(wrap(
        "var i := 0; "
        "var acc := 0; "
        "while i < 100 do "
        "  var x := new Res(5); "
        "  var y := move x; "
        "  y.Dispose(); "
        "  acc := acc + 1; "
        "  i := i + 1; "
        "end; "
        "return acc - 100;"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(jr));
    // Pre-fix: LSan catches the leak (one heap block per loop iteration,
    // 100 of them) and `jr.ran` becomes false. Post-fix: the explicit
    // Dispose reclaims each block, LSan is silent, `jr.ran` stays true.
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
    // Exactly 100 destructors — one per `new Res(5)`. An over-fire (double
    // dispose) reads 200; an under-fire (the leak) reads 100 too, because
    // the destructor DID run, so this assertion is necessary but not
    // sufficient — the leak-vs-not decision is the `jr.ran` check above
    // (LSan flips it on a leak).
    BEZEL_EXPECT_EQ(countOccurrences(jr.stdoutText, "D"), 100);
}

// ============================================================================
// LEAK DIRECTION — counting prelude that bumps a process-global counter so
// a future over-firing fix (double-dispose on the fix's path) is caught as
// a wrong NUMBER, not as a missing crash.
// ============================================================================

const char* kCountingPrelude =
    "module M; "
    "class Counter "
    "  public static Dropped: int := 0; "
    "end Counter; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  destructor Counter.Dropped := Counter.Dropped + 1; end destructor; "
    "end Res; "
    "class C "
    "  public static method Main(): int ";

std::string wrapCounting(const std::string& body) {
    return std::string(kCountingPrelude) + body + kEpilogue;
}

// 50 iters (a small count to keep the exit code in 8-bit range). The
// destructor fires on the explicit Dispose (the slot is nulled afterwards
// so the scope-end reload sees null and skips). An over-free reads 100;
// the *1 amplifies to distinguish. Under a pre-fix build the destructor
// fires exactly once per `new Res(5)` (the leak is on the heap block, not
// on the destructor call), so this is the load-bearing "exactly-once"
// oracle for the explicit-Dispose arm.
BEZEL_TEST(explicit_dispose_move_init_owned_local_leak_3968,
           reclaim_is_exactly_once_counting_28_2) {
    JitResult jr = runAot(wrapCounting(
        "var i := 0; "
        "var acc := 0; "
        "while i < 50 do "
        "  var x := new Res(5); "
        "  var y := move x; "
        "  y.Dispose(); "
        "  acc := acc + 1; "
        "  i := i + 1; "
        "end; "
        "return Counter.Dropped + (acc - 50);"));
    BEZEL_EXPECT_TRUE(jr.ran);
    // 50 destructions exactly — one per `new Res(5)`. The destructor
    // fires on the explicit Dispose; the slot is nulled so the scope-end
    // reload sees null and skips. An over-free reads 100 (50 + 50 from
    // the now-non-null reload).
    BEZEL_EXPECT_EQ(jr.exitCode, 50);
}

// ============================================================================
// POSITIVE CONTROLS — the issue's table of clean rows must STAY clean on
// both sides of the fix. Each variant counts the destructors.
// ============================================================================

// `var x := new Res(5); x.Dispose();` in a 50-iter loop — pre-fix already
// clean (this was the working arm that taught us the auto-drop was fine
// for plain `new`-init). Post-fix: still clean. The fix must not regress
// the working arm.
BEZEL_TEST(explicit_dispose_move_init_owned_local_leak_3968,
           plain_new_init_explicit_dispose_still_works_28_3) {
    JitResult jr = runAot(wrapCounting(
        "var i := 0; "
        "var acc := 0; "
        "while i < 50 do "
        "  var x := new Res(5); "
        "  x.Dispose(); "
        "  acc := acc + 1; "
        "  i := i + 1; "
        "end; "
        "return Counter.Dropped + (acc - 50);"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 50);
}

// `var x := new Res(5); var y := move x;` (no explicit Dispose) in a
// 50-iter loop — auto-drop only. Pre-fix: clean (auto-drop path reclaims
// correctly). Post-fix: still clean. The fix added an arm to
// `unwindOwnedLocalClass`, but the auto-drop path is keyed on the TYPE
// through `registerUsingDisposal`, not on `unwindOwnedLocalClass`, so the
// two paths don't interact — and this row guards that contract.
BEZEL_TEST(explicit_dispose_move_init_owned_local_leak_3968,
           move_init_no_explicit_dispose_auto_drop_only_28_3) {
    JitResult jr = runAot(wrapCounting(
        "var i := 0; "
        "var acc := 0; "
        "while i < 50 do "
        "  var x := new Res(5); "
        "  var y := move x; "
        "  acc := acc + 1; "
        "  i := i + 1; "
        "end; "
        "return Counter.Dropped + (acc - 50);"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 50);
}

// `var x := new Res(5); x.Dispose(); x := new Res(7); x.Dispose();` — the
// dispose-then-reassign-then-dispose pattern from the issue's clean row.
// Pre-fix: clean. Post-fix: still clean. 2 disposes per iter × 50 iters
// = 100 destructions total.
BEZEL_TEST(explicit_dispose_move_init_owned_local_leak_3968,
           dispose_then_reassign_then_dispose_28_3) {
    JitResult jr = runAot(wrapCounting(
        "var i := 0; "
        "var acc := 0; "
        "while i < 50 do "
        "  var x := new Res(5); "
        "  x.Dispose(); "
        "  x := new Res(7); "
        "  x.Dispose(); "
        "  acc := acc + 1; "
        "  i := i + 1; "
        "end; "
        "return Counter.Dropped + (acc - 50);"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 100);
}

// ============================================================================
// OWNERSHIP-PASS POSITIVE — the program must STILL be accepted by the
// ownership checker. The fix is a codegen gate, not a new rejection, and a
// future over-rejection of `var y := move x; y.Dispose();` would surface
// here as a diagnostic that isn't supposed to be there.
// ============================================================================

BEZEL_TEST(explicit_dispose_move_init_owned_local_leak_3968,
           ownership_check_stays_clean_28_3) {
    OwnershipResult r = parseAndCheckOwnership(wrap(
        "var x := new Res(5); "
        "var y := move x; "
        "y.Dispose(); "
        "return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ============================================================================
// §28.2 IDEMPOTENCE — `y.Dispose(); y.Dispose();` on the same move-init
// owned local. The SECOND call must be a no-op (the slot was nulled by
// the first), so exactly ONE destructor fires. An over-firing fix that
// did not null the slot would dispatch the destructor a SECOND time on a
// freed block — a use-after-free, observable here as a destructor count
// of 2 instead of 1.
// ============================================================================

BEZEL_TEST(explicit_dispose_move_init_owned_local_leak_3968,
           double_dispose_is_idempotent_not_a_double_free_28_2) {
    JitResult jr = runAot(wrapCounting(
        "var x := new Res(5); "
        "var y := move x; "
        "y.Dispose(); "
        "y.Dispose(); "
        "return Counter.Dropped;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    // Exactly ONE destructor fires — the run-once flag makes the second
    // dispatch a no-op AND the slot-null makes the second reload see null
    // (the conditional null-guard around `emitDisposeAndReclaim` skips the
    // whole arm).
    BEZEL_EXPECT_EQ(jr.exitCode, 1);
}
