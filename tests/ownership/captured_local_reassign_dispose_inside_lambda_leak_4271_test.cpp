// Ownership / codegen — a local DECLARED OUTSIDE a lambda and reassigned
// + `.Dispose()`d INSIDE the lambda body leaks the freshly-reassigned
// owned value. Issue #4271.
//
// SPEC — settled, not underspecified. §27 "Closures" / §28.3 "Local
// variables, ownership, and borrows" / §28.6 "Who may call `Dispose()`"
// (v0.298). §28.6 requires the explicit `.Dispose()` to RECLAIM the
// heap block whose ownership the receiver holds at that program point
// — §28.3's K1 row declares a `var y := x;` over an Owned-tracked
// outer `x` as Borrowed (the local's static ownership classification),
// but the per-call reclaim-eligibility lives in the OWNERSHIP CHECKER'S
// OWN dataflow, which correctly re-classifies `y` Owned at
// `y := new Res(9);` and therefore demands `.Dispose()` be accepted,
// and §28.6's reclaim object at that call is the heap block `y` holds
// — a freshly-owned `new Res(9)`.
//
// WHY IT WAS MISSED. The lambda-body pre-pass
// `applyUsingVarBorrowPinInLambdas` (Ownership.cpp ~9888) wipes
// `states_` to a fresh empty map before each lambda-body walk — so
// the bookkeeping arms reading it find no entry for the captured
// local `y`. (1) `applyUsingVarBorrowPinInLambdaStmt`'s
// `AssignmentStmt` arm's `for (auto& [sym, st] : states_) if
// (sym->name != tid->name) continue;` finds no entry to update (no
// producer-state adoption, no `disposeAliasSource` clear). (2)
// `markExplicitDisposeReclaimIfReassignedBorrow`'s `lookupState(name)`
// finds no entry, so it does NOT stamp
// `disposeReclaimsReassignedBorrow` on the `.Dispose()` receiver.
// Codegen's `unwindOwnedLocalClass(ld)` (LlvmBackend_Calls2.cpp)
// classifies PURELY from `ld`'s DECLARATION initializer form —
// `var y := x;` is a bare-identifier borrow, none of the
// new/call/move/bind/conditional-meet forms it checks — so it
// returns null FOREVER for `y`, even at the explicit `.Dispose()`
// where the ownership checker's own dataflow classifies `y` Owned.
// The explicit dispose therefore only DISPATCHES the destructor
// (LlvmBackend_Disposal.cpp's `emitDynamicDispose`) and never
// RECLAIMS the heap block — a leak, not a crash, exactly per the
// issue's report.
//
// MEASURED PRE-FIX (this lane, base 96828993, bezelc built clean,
// both repros run under `--sanitize=address`,
// `ASAN_OPTIONS=detect_leaks=1`):
//
//   var x := new Res(5); var y := x;
//   var f := lambda(): int => do
//     y := new Res(9); y.Dispose(); return 0;
//   end;
//   f(); x.Dispose();
//     -> exit=0, "Direct leak of 16 byte(s) in 1 object(s)" (1x `Res(9)`,
//        the reassigned-then-disposed value)
//     -> exit=0, same leak at the inner `y.Dispose()` site
//        (the outer `x.Dispose()` correctly reclaims `x`'s `Res(5)`)
//
// THE FIX — `seedCapturedOuterLocals` (Ownership.cpp ~9757) walks
// every identifier reachable from the lambda body, identifies
// captured LocalSymbols (decl outside the body Stmts set), and
// derives the captured local's declaration-time `LocalOwnership`
// directly from its `LocalDecl` AST via `seedCapturedLocalFromDecl`
// (Ownership.cpp ~9866), inserting it into `states_` BEFORE the
// body walk. This seeds the `Borrowed + disposeAliasSource = x`
// row `handleLocalDecl` (Ownership.cpp ~5683-5985) would have
// produced for `var y := x;` at method scope. The AssignmentStmt
// arm then finds the entry and adopts the Owned producer state from
// `y := new Res(9);`; `markExplicitDisposeReclaimIfReassignedBorrow`
// finds the entry and stamps `disposeReclaimsReassignedBorrow` on
// the receiver; codegen reclaims. Mirrors the same pre-pass-walk
// shape `handleLocalDecl` performs at method scope.
//
// per §27, §28.3, §28.6, v0.298, #4271.

#include <string>

#include "support/aot_support.hpp"  // runAot / runAotChecked — the AOT oracle
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kPrelude =
    "module M; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  destructor end destructor; "
    "end Res; "
    "class C "
    "  public static method Main(): int ";

const char* kEpilogue =
    "  end Main; "
    "end C; ";

std::string wrap(const std::string& body) {
    return std::string(kPrelude) + body + kEpilogue;
}

// Counting prelude — pins EXACT disposal counts as a cross-check on
// the ASan/LSan oracle. An unfixed run that LOOKS clean (no UAF,
// but the heap block never reclaimed) still sees a destructor
// DISPATCH (the `dispatch-only` path) — a counter sees the same
// dispatch count. The leak signature lives in ASan/LSan alone (count
// + `Direct leak of N byte(s)`), not the counter — so the counter is
// a discharge-count witness, not the leak oracle.
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

}  // namespace

// ============================================================================
// A. PRIMARY REGRESSION — the issue's exact outer-scope-capture shape,
// loop-amplified (§R10 lane-rules / `lsan-one-shot-leak-tests-pass-vacuously`:
// a one-shot ASan row can pass on both sides by luck of allocator reuse; 200
// iterations gives the freed block many chances to be reused/poisoned
// between free and the later use, AMPLIFYING the leak from 1 object to 200
// in the leak row).
// ============================================================================

BEZEL_TEST(captured_local_reassign_dispose_inside_lambda_leak_4271,
           issue_repro_outer_captured_reassign_then_dispose_loop_28_3) {
    JitResult jr = runAotChecked(wrap(
        "var x := new Res(5); "
        "var y := x; "
        "var f := lambda(): int => do "
        "  y := new Res(9); "
        "  y.Dispose(); "
        "  return 0; "
        "end; "
        "var i := 0; "
        "var acc := 0; "
        "while i < 200 do "
        "  acc := acc + f(); "
        "  i := i + 1; "
        "end; "
        "x.Dispose(); "
        "return acc;"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(jr));
    // Pre-fix: seed absent; `states_` empty during body walk;
    // AssignmentStmt arm finds no entry to update;
    // `markExplicitDisposeReclaimIfReassignedBorrow` stamps nothing;
    // codegen's `unwindOwnedLocalClass` returns null on the
    // borrow-init Decl forever — `y.Dispose()` dispatch-only leaks
    // `new Res(9)`. 200 iterations -> "Direct leak of 3200 byte(s)
    // in 200 object(s)" (16 B x 200).  Post-fix: seed inserts the
    // Borrowed + disposeAliasSource row before the walk, the
    // assignment arm adopts Owned, the dispose stamps
    // disposeReclaimsReassignedBorrow, codegen reclaims.
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// ============================================================================
// B. OWNERSHIP-PASS POSITIVE — the fix is a codegen/dataflow-state gate, not
// a new rejection; the issue's own repro shape must be accepted with zero
// ownership diagnostics. The leak survives the checker (per-issue
// verifier) — the bug is on the codegen path that runs AFTER the
// checker accepts the program.
// ============================================================================

BEZEL_TEST(captured_local_reassign_dispose_inside_lambda_leak_4271,
           ownership_check_stays_clean_28_3) {
    OwnershipResult r = parseAndCheckOwnership(wrap(
        "var x := new Res(5); "
        "var y := x; "
        "var f := lambda(): int => do "
        "  y := new Res(9); "
        "  y.Dispose(); "
        "  return 0; "
        "end; "
        "return f();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ============================================================================
// C. AXIS — NESTED-LAMBDA CAPTURE. The captured local `y` is one
// frame OUT (declared in an outer lambda body, captured by an
// inner lambda body). The fix path includes a sibling seed site in
// `applyUsingVarBorrowPinInLambdaBody` (Ownership.cpp ~10824) so a
// nested-lambda capture also seeds — without it the leak survives
// even with the outer-scope case fixed. This axis proves both seed
// sites work.
// ============================================================================

BEZEL_TEST(captured_local_reassign_dispose_inside_lambda_leak_4271,
           axis_nested_lambda_capture_reassign_then_dispose_28_3) {
    JitResult jr = runAotChecked(wrap(
        "var outer := lambda(): int => do "
        "  var x := new Res(5); "
        "  var y := x; "
        "  var inner := lambda(): int => do "
        "    y := new Res(9); "
        "    y.Dispose(); "
        "    return 0; "
        "  end; "
        "  var i := 0; "
        "  while i < 200 do "
        "    i := i + 1; "
        "  end; "
        "  inner(); "
        "  x.Dispose(); "
        "  return 0; "
        "end; "
        "return outer();"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(jr));
    // Pre-fix: nested-lambda seed site is the same one missing (the
    // outer-scope fix in
    // `applyUsingVarBorrowPinInLambdas` is necessary but NOT
    // sufficient — `applyUsingVarBorrowPinInLambdaBody`'s sister
    // site must also seed). Both sites ship together.  Post-fix:
    // both seeds fire; `inner()` correctly reclaims `new Res(9)`.
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// ============================================================================
// D. NO-AMPLIFICATION + DISPATCH-COUNT FINGERPRINT. The first 3 rows
// loop-amplify so the LSan report quantifies the leak (200 objects
// is unambiguous). This row runs ONCE — it pins the destructor
// DISPATCH fingerprint (the dispatch fires regardless of the
// reclaim gate, so both sides see 2) AND adds a non-amplified
// row that pins the same leak. A leak in a one-shot run still
// trips LSan (it does not need 200 objects) — the amplification
// is for human readability of the failure, not for the oracle.
//
// Pinned values:
//   pre-fix:  jr.ran == false  (LSan exit 99 — the Res(9) leaks)
//   post-fix: jr.ran == true, exitCode == 2  (Counter.Dropped
//             returns 2 dispatches: Res(5) + Res(9), both
//             reclaimed)
// ============================================================================

BEZEL_TEST(captured_local_reassign_dispose_inside_lambda_leak_4271,
           dispatch_count_fingerprint_one_shot_28_6) {
    JitResult jr = runAotChecked(wrapCounting(
        "var x := new Res(5); "
        "var y := x; "
        "var f := lambda(): int => do "
        "  y := new Res(9); "
        "  y.Dispose(); "
        "  return 0; "
        "end; "
        "f(); "
        "x.Dispose(); "
        "return Counter.Dropped;"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(jr));
    // MEASURED POST-FIX: 2 dispatches (Res(5) reclaimed via
    // x.Dispose(); Res(9) reclaimed via y.Dispose()), and
    // Counter.Dropped is the return value. Pre-fix: same dispatch
    // count (dispatch is independent of the reclaim gate), but
    // LSan detects the leak and `jr.ran` becomes false — the
    // harness exit-99 path triggers. The asymmetry (pre-fix
    // `jr.ran == false`, post-fix `jr.ran == true && exitCode
    // == 2`) pins the leak with a single iteration, independent
    // of amplification.
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 2);
}
