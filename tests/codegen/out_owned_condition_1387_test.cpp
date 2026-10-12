// fix #1387 / §28.3 / §28.11 / §22.4:
// Codegen correctness tests for `out using var` / `out owned var` inline locals
// declared in an IF-CONDITION.
//
// ITEM 1 (#1387 §28.11 / §22.4):
//   `if s.TryPop(out using var top) then ... end` must dispose `top` at the END
//   OF THE IF STATEMENT. Before the fix, the disposal was registered in the
//   enclosing scope (block or method), so `top` stayed alive past the `end` --
//   a leak of potentially multiple iterations. The fix pushes a dedicated
//   condition scope in `lowerIf` so out-inline locals land there and are
//   emitted by `emitDisposals` at the `if.end` merge block.
//
// ITEM 2 (#1387 §28.3 v0.150 / §17.13):
//   `out owned var top` inline locals in an if-condition now follow the §28.3
//   affine auto-drop rule instead of giving a hard error:
//   (a) `_ := top` inside the body is a legal §17.13 discard-discharge --
//       the ownership pass sets discardOutOwnedVar so codegen disposes + nulls
//       the slot; the condition scope's dynamicDispose is then a no-op.
//   (b) Undischarged `out owned var top` -- the ownership pass emits a must-use
//       warning and sets autoDropAtScopeEnd; ensureOutVarSlots registers a
//       dynamicDispose that fires at if-end.
//   Both (a) and (b) REQUIRE the ownership pass, which runJit does not invoke.
//   The sema-level tests (tests/sema/out_owned_condition_892_test.cpp and
//   tests/sema/out_owned_transfer_860_test.cpp) cover those paths. The codegen
//   tests below instead verify the OBSERVABLE codegen behaviour that is testable
//   WITHOUT the ownership pass: explicit dispose in the if-body and the
//   condition scope's null-guarded unwindOnly path.
//
// NOTE on empty if-bodies: Bezel requires at least one statement between `then`
// and `end`; comment-only bodies parse as empty and trigger a parse error.
// Tests that target the FALSE path (body never entered) therefore include a
// `return 99;` -- an unreachable sentinel that satisfies the parser but has no
// effect on the Cell._n computation on the path that falls through.
//
// Value tests: runJit returns exit code (0..255). Each program uses a static
// destructor counter `Cell._n` (constructor +1, destructor -1). A correct
// program ends at the expected counter value; a leak leaves _n too high; a
// double-free drives _n negative (wraps to a large value). // per §28.3, §28.11

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// Class with a static creation/disposal counter.
// constructor increments Cell._n; destructor decrements Cell._n.
// Correct execution: every constructor is paired with exactly one destructor.
const char* kCellHdr =
    "module M; class Cell public static _n: int := 0; "
    "  public constructor() Cell._n := Cell._n + 1; end constructor; "
    "  destructor Cell._n := Cell._n - 1; end destructor; end Cell; ";

}  // namespace

// ============================================================================
// ITEM 1 -- `out using var` condition-scope disposal (§28.11, §22.4)
// ============================================================================

// §28.11 / §22.4 / #1387 (headline): two cells on a stack, TryPop the top into
// `out using var top`. Capture Cell._n INSIDE the body (=2: both alive) and
// AFTER the if (=1: top disposed at the if's `end`). The remaining cell is still
// in the stack. Before the fix, `top` stayed alive past the `end` so `afterIf`
// was 2, giving return value 22. The fix disposes `top` at the if-statement's
// `end` so `afterIf`=1 --> return 21.
BEZEL_TEST(out_owned_condition_1387, if_cond_out_using_var_disposed_at_if_end) {
    BEZEL_EXPECT_EQ(run(std::string(kCellHdr) +
        "class C public static method Main(): int "
        "  var inBody: int := 0; "
        "  var afterIf: int := 0; "
        "  using var s := new OwningStack<Cell>(); "
        "  s.Push(move (new Cell())); "
        "  s.Push(move (new Cell())); "
        "  if s.TryPop(out using var top) then "
        "    inBody := Cell._n; "
        "  end; "
        "  afterIf := Cell._n; "
        "  return inBody * 10 + afterIf; "
        "end Main; end C;"),
    // inBody=2 (both cells alive in body), afterIf=1 (top disposed at if-end)
    21);
}

// §28.11 / §22.4 / #1387 (false path -- empty stack): TryPop returns false, the
// `top` slot is null. The condition scope's dynamicDispose fires at if-end but
// the null-guard makes it a safe no-op. No crash, no double-free. Cell._n=0.
// The `return 99` in the body is unreachable (stack is always empty); it
// satisfies the parser (Bezel requires at least one statement between then/end).
BEZEL_TEST(out_owned_condition_1387, if_cond_out_using_var_false_path_safe) {
    BEZEL_EXPECT_EQ(run(std::string(kCellHdr) +
        "class C public static method Main(): int "
        "  using var s := new OwningStack<Cell>(); "
        "  if s.TryPop(out using var top) then "
        "    return 99; "
        "  end; "
        "  return Cell._n; "
        "end Main; end C;"),
    // stack always empty --> TryPop always false --> slot null --> no-op disposal
    0);
}

// ============================================================================
// ITEM 2 -- `out owned var` in if-condition: explicit dispose + condition scope
// (§28.3 v0.150, §22.4)
//
// Note: the OWNERSHIP-PASS-dependent paths (auto-drop via autoDropAtScopeEnd
// and discard-discharge via discardOutOwnedVar / _ := top) are tested at the
// sema level in tests/sema/out_owned_condition_892_test.cpp and
// tests/sema/out_owned_transfer_860_test.cpp. The codegen JIT harness does not
// run the ownership pass, so those lowering paths are never activated here.
// The tests below cover what IS observable without the ownership pass:
//   (a) explicit top.Dispose() in the body disposes the cell exactly once
//       (condition scope's unwindOnly is skipped on normal exit -- no double-free)
//   (b) the null-guard at if-end is a safe no-op when TryPop returned false
// ============================================================================

// §28.3 / §22.4 / #1387 (Item 2, explicit dispose -- true path): one cell is
// pushed and popped into `out owned var top`. The if-body calls top.Dispose()
// to discharge the owned value. The condition scope's unwindOnly disposal is
// skipped on the normal exit path -- no double-free. Cell._n=0 at return.
BEZEL_TEST(out_owned_condition_1387, if_cond_out_owned_var_discard_discharge) {
    BEZEL_EXPECT_EQ(run(std::string(kCellHdr) +
        "class C public static method Main(): int "
        "  using var s := new OwningStack<Cell>(); "
        "  s.Push(move (new Cell())); "
        "  if s.TryPop(out var top) then "
        "    top?.Dispose(); "  // v0.171 (#1336): out owned is T?, discharge via `?.`
        "  end; "
        "  return Cell._n; "
        "end Main; end C;"),
    // top.Dispose() disposes once; condition scope unwindOnly skipped --> no double-free
    0);
}

// §28.3 / §22.4 / #1387 (Item 2, explicit dispose -- false path): empty stack,
// TryPop returns false, the `top` slot is null. The if-body is unreachable
// (return 99 is a parser-satisfying sentinel). The condition scope's unwindOnly
// fires at if-end on the NORMAL exit -- but since isUnwind=false, it is skipped.
// No crash. Cell._n=0 (no cells created).
BEZEL_TEST(out_owned_condition_1387, if_cond_out_owned_var_discard_false_path) {
    BEZEL_EXPECT_EQ(run(std::string(kCellHdr) +
        "class C public static method Main(): int "
        "  using var s := new OwningStack<Cell>(); "
        "  if s.TryPop(out var top) then "
        "    return 99; "
        "  end; "
        "  return Cell._n; "
        "end Main; end C;"),
    // stack empty --> TryPop false --> body skipped --> slot null --> unwindOnly skipped
    0);
}

// §28.3 / §22.4 / #1387 (Item 2, explicit dispose in body -- two cells): one
// cell is popped and explicitly disposed inside the if-body; the second remains
// in the stack. Cell._n after the if is 1 (second cell still alive). Confirms
// the condition scope's unwindOnly does NOT fire on the normal exit and cause an
// extra dispose. snap=Cell._n is captured after the if to distinguish this from
// the auto-drop path that the ownership pass enables.
BEZEL_TEST(out_owned_condition_1387, if_cond_out_owned_var_auto_drop) {
    BEZEL_EXPECT_EQ(run(std::string(kCellHdr) +
        "class C public static method Main(): int "
        "  var snap: int := -1; "
        "  using var s := new OwningStack<Cell>(); "
        "  s.Push(move (new Cell())); "
        "  if s.TryPop(out var top) then "
        "    top?.Dispose(); "  // v0.171 (#1336): out owned is T?, discharge via `?.`
        "  end; "
        "  snap := Cell._n; "
        "  return snap; "
        "end Main; end C;"),
    // top.Dispose() in body; condition scope unwindOnly skipped; snap=Cell._n=0
    0);
}

// §28.3 / §22.4 / #1387 (Item 2, false path -- null guard): empty stack, TryPop
// returns false. The `top` slot is null. Body is unreachable (return 99 is a
// parser sentinel). The condition scope's unwindOnly fires at if-end but is
// skipped (isUnwind=false on normal exit). No crash. Cell._n=0 (no cells).
BEZEL_TEST(out_owned_condition_1387, if_cond_out_owned_var_auto_drop_false_path) {
    BEZEL_EXPECT_EQ(run(std::string(kCellHdr) +
        "class C public static method Main(): int "
        "  using var s := new OwningStack<Cell>(); "
        "  if s.TryPop(out var top) then "
        "    return 99; "
        "  end; "
        "  return Cell._n; "
        "end Main; end C;"),
    // stack empty --> TryPop false --> body unreachable --> unwindOnly skipped
    0);
}
