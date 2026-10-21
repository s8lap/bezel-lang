// Ownership — `MethodOwnership::checkRead` is the ONE authority that detects a
// MovedFrom / Disposed / maybe-consumed / Uninitialized local at a READ
// position, and its expression recursion stopped at nine node kinds, falling
// off the end with "Other expression forms — not exhaustive; later slices
// extend."  Eleven ordinary node kinds sat in that remainder, so a read of an
// already moved-from (or explicitly disposed) owned local nested inside one of
// them was completely undiagnosed in plain STRAIGHT-LINE code — and because
// every caller (a `var` initializer, a `const` initializer, an assignment RHS,
// a `return` operand, a bare statement, a loop body) funnels through this one
// function, each missing arm was a hole at ALL of them at once.  That is what
// distinguishes this family from #3591 / #3687 / #3864 / #3931 / #3949, each of
// which was a hole in ONE caller.  Issue #3971.
//
// SPEC — settled, not underspecified.  §28.3 "Local variables, ownership, and
// borrows", K1 "Value classification", item 3 (v0.294), verbatim:
//
//   "**moved-from** — the obligation has transferred out; the binding or slot
//    is unreadable until reassigned, and discharge machinery skips it"
//
// and the same list's auxiliary state:
//
//   "**explicitly-disposed** — the binding after a site-announced kill that
//    moves no obligation ... and a straight-line read falls under [§28.2]'s
//    use-after-dispose warning regime rather than the moved-from error."
//
// §28.2 "The `IDisposable` contract" (v0.294): "The compiler additionally
// **must** emit a compile-time warning when a local variable is used (read,
// borrowed from, or consumed) after an explicit `Dispose()` call ... on the
// same variable that dominates the use site on every path".
//
// K7 "The join rule" (v0.294) names "`if` expressions, expression-position
// `case` arms" among the ordinary control-flow joins.  Nothing in either
// section carves out a constructor argument, an `if`-expression arm, a `case`
// scrutinee/guard/arm, a collection-literal element, an interpolation hole, a
// `?.` receiver, an `is` operand, a `with` source, a range endpoint or a
// `throw` operand.
//
// MEASURED PRE-FIX on this branch (origin/main e240743 — i.e. WITH #3864,
// #3931, #3949 and #3950 all landed).  Every body begins
// `var x := new Res(1); var t := Sink.Take(move x);` — `Take` never discharges
// its `owned` parameter, so §28.3's auto-drop frees the moved-in value at the
// end of `Take` and `x` is provably dangling once `Take` returns:
//
//   | read position                              | rc | diagnostics | ASan     |
//   |--------------------------------------------|----|-------------|----------|
//   | `Sink.Peek(x);`         (bare stmt, CTRL)  |  1 | §28.3 error | —        |
//   | `var u := Sink.Peek(x);`      (var,  CTRL) |  1 | §28.3 error | —        |
//   | `return t + x.tag;`        (return,  CTRL) |  1 | §28.3 error | —        |
//   | `new Res(x.tag)`   (all 5 caller spellings)|  0 | **NONE**    | heap-UAF |
//   | `(if c then x.tag else 0)`  then/else/cond |  0 | **NONE**    | heap-UAF |
//   | `(case x.tag of …)`     scrutinee/guard/arm|  0 | **NONE**    | heap-UAF |
//   | `[x.tag]`              collection literal  |  0 | **NONE**    | heap-UAF |
//   | `$"{x.tag}"`           interpolation hole  |  0 | **NONE**    | heap-UAF |
//   | `x is Res`                      type test  |  0 | **NONE**    | heap-UAF |
//   | `xn?.tag` / `xn?.Get()`  null-cond, WITHHELD|  0 | **NONE**    | heap-UAF |
//   | `p with (A := x.tag)`     with-expression  |  0 | **NONE**    | heap-UAF |
//   | `x.tag..3`                    range        |  0 | **NONE**    | heap-UAF |
//   | `throw Sink.MkEx(x.tag)`   throw operand   |  0 | **NONE**    | heap-UAF |
//
// 17 read positions across 11 node kinds, every one a silent accept whose
// program heap-use-after-frees under `--sanitize=address` ("READ of size 4",
// freed frame `bz$M.Sink$Take`), while the byte-identical read one node kind
// over is correctly rejected.  The DISPOSE twin behaved identically: §28.2's
// "use of `x` after a dominating explicit Dispose() call" fired for the walked
// positions and was silent for all eleven un-walked ones.
//
// ROOT CAUSE: `checkRead`'s recursion covered `BindExpr`, a MARKED
// `ConditionalExpr`/`CaseExpr`, `IdentifierExpr`, `MemberAccessExpr`,
// `BinaryExpr`, `CallExpr`, `UnaryExpr`, `PostfixBangExpr` and `IndexExpr` and
// nothing else.  `NewExpr`, a marker-free `ConditionalExpr`/`CaseExpr`,
// `CollectionLiteralExpr`, `InterpolatedStringExpr`, `NullCondMemberExpr`,
// `NullCondCallExpr`, `TypeTestExpr`, `WithExpr`, `RangeExpr` and `ThrowExpr`
// were all in the un-walked remainder.
//
// SCOPE — 15 of the 17 positions, 9 of the 11 node kinds.  The `?.` pair
// (`NullCondMemberExpr` / `NullCondCallExpr`) is measured, its four-line arms
// do close both repros, and it is still WITHHELD: `?.` is the last read
// spelling that escapes issue 3983, a PRE-EXISTING false diagnostic in which a
// nullable owned local's plain null-CHECK arms `disposedOnSomePath`.  Adding
// the arms extends that false diagnostic to the canonical `?.Dispose()`
// discharge and breaks six correct shipped tests
// (owned_collection_trypop_831 x4, owned_set_take_1476 x2).  Filed as issue
// 3984, ordered behind 3983.  See the comment at the arms' site in
// Ownership_Checks.cpp.
//
// THE FIX extends the existing walk (it does not add a parallel one) with one
// arm per remaining missing node kind.  Every arm is a pure READ walk over
// subexpressions that are already read positions; none consumes, marks or
// transfers anything.
//
// ⚠ THE #3941 HAZARD, and why the guard rows below matter.  `checkRead` runs
// BEFORE `recordMovesIn` on every binding path and its report POISONS the
// record, after which `markMovedByName` (which has no Poisoned arm, by design)
// silently falls through — so a read-check placed over a CONSUMING operand
// steals that operand's more precise diagnostic.  Measured pre-fix:
// `x.Dispose(); using var h := new Holder(move x);` reports the ERROR "cannot
// move from disposed local `x` (§28.3)" and REJECTS.  A naive `NewExpr` arm
// that simply walked every argument would degrade that to §28.2's WARNING and
// ACCEPT the program.  So the new arms route operands through a guard that
// declines the top-level consuming forms — via the shared
// `isTopLevelConsumingForm` predicate, promoted from Ownership.cpp's anonymous
// namespace to Ownership_impl.hpp so all three callers ask ONE spelling — and
// the `NewExpr` arm additionally declines a bare identifier bound to an
// `owned`/`out owned` ctor formal (the §28.8 implicit transfer, whose
// diagnostic `recordMovesIn`'s own NewExpr branch owns).  The four
// `guard_*` rows below pin that those programs keep their PRE-FIX, more
// precise diagnostic.
//
// FALSE-REJECT COST: 0 measured.  Ten legitimate programs — one per newly
// walked node kind, plus the marked-arm transfer — compile and produce
// byte-identical runtime values against the pre-fix compiler; they are pinned
// below as value-asserting positives.
//
// per §28.3 K1.3/K7, §28.2, v0.294, #3941, #3971

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot — the AOT oracle
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

bool diagWithSeverity(const OwnershipResult& r, bezel::Severity sev,
                      const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != sev) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// True iff the diagnostic carries a real source location — never 0:0.
bool diagHasRealLocation(const OwnershipResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find(needle) == std::string::npos) continue;
        if (d.location.line != 0 && d.location.column != 0) return true;
    }
    return false;
}

// How many diagnostics carry `needle` — one, never two: the new read-check
// must not double-report alongside an existing, more specific check.
int diagCount(const OwnershipResult& r, const std::string& needle) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) ++n;
    return n;
}

const char* kPrelude =
    "module M; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  public method Get(): int return this.tag; end Get; "
    "  destructor end destructor; "
    "end Res; "
    "record Pt(A: int, B: int); "
    "class Sink "
    "  public static method Take(owned r: Res): int return r.tag; end Take; "
    "  public static method Peek(r: Res): int return r.tag; end Peek; "
    "  public static method Id(n: int): int return n; end Id; "
    "  public static method MkEx(n: int): owned InvalidOperationException "
    "    return new InvalidOperationException(\"e\"); "
    "  end MkEx; "
    "end Sink; "
    "class Holder "
    "  public v: int; "
    "  public constructor(owned r: Res) this.v := r.tag; end constructor; "
    "  destructor end destructor; "
    "end Holder; ";

std::string wrap(const std::string& body) {
    return std::string(kPrelude) +
           "class C "
           "  public static method Main(): int " +
           body +
           "  end Main; "
           "end C;";
}

// The shared first two lines of every use-after-MOVE row.  `Take` never
// discharges its `owned` parameter, so §28.3's auto-drop frees the moved-in
// value at the end of `Take` and `x` is dangling from the third line on.
const char* kConsumed =
    "var x := new Res(1); "
    "var t := Sink.Take(move x); ";

// The shared first lines of every use-after-DISPOSE row.
const char* kDisposed =
    "var x := new Res(1); "
    "var t := 0; "
    "x.Dispose(); ";

const char* kMovedOut =
    "owned value in `x` was already moved out at this point (§28.3)";
const char* kUseAfterDispose =
    "use of `x` after a dominating explicit Dispose() call";
const char* kMoveFromDisposed =
    "cannot move from disposed local `x` (§28.3)";

// One rejected row: exactly one §28.3 moved-out ERROR, at a real location.
void expectMovedOutOnce(const std::string& body,
                        const char* needle = kMovedOut) {
    OwnershipResult r = parseAndCheckOwnership(wrap(body));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, needle));
    BEZEL_EXPECT_TRUE(diagHasRealLocation(r, needle));
    BEZEL_EXPECT_EQ(diagCount(r, needle), 1);
}

}  // namespace

// ============================================================================
// CONTROLS — the spellings that were ALREADY rejected before this fix.  They
// anchor the asymmetry every negative row below measures.
// ============================================================================

BEZEL_TEST(checkread_expression_walk_3971,
           control_bare_statement_read_after_move_rejected_28_3) {
    expectMovedOutOnce(std::string(kConsumed) + "Sink.Peek(x); return t;");
}

BEZEL_TEST(checkread_expression_walk_3971,
           control_var_initializer_read_after_move_rejected_28_3) {
    expectMovedOutOnce(std::string(kConsumed) +
                       "var u := Sink.Peek(x); return t + u;");
}

BEZEL_TEST(checkread_expression_walk_3971,
           control_return_operand_read_after_move_rejected_28_3) {
    expectMovedOutOnce(std::string(kConsumed) + "return t + x.tag;");
}

// ============================================================================
// NEGATIVE — `NewExpr` constructor arguments.  Four caller spellings, because
// the defect is in `checkRead` itself and therefore hits every caller at once.
// FAIL WITHOUT THE FIX: zero diagnostics, and the program heap-use-after-frees.
// ============================================================================

BEZEL_TEST(checkread_expression_walk_3971,
           new_ctor_argument_in_var_initializer_rejected_28_3) {
    expectMovedOutOnce(std::string(kConsumed) +
                       "using var q := new Res(x.tag); return t + q.tag;");
}

BEZEL_TEST(checkread_expression_walk_3971,
           new_ctor_argument_in_assignment_rhs_rejected_28_3) {
    expectMovedOutOnce(std::string(kConsumed) +
                       "var q := new Res(0); q := new Res(x.tag); "
                       "var v := q.tag; q.Dispose(); return t + v;");
}

BEZEL_TEST(checkread_expression_walk_3971,
           new_ctor_argument_in_return_operand_rejected_28_3) {
    expectMovedOutOnce(std::string(kConsumed) +
                       "return t + (new Res(x.tag)).tag;");
}

BEZEL_TEST(checkread_expression_walk_3971,
           new_ctor_argument_in_bare_statement_rejected_28_3) {
    expectMovedOutOnce(std::string(kConsumed) +
                       "Sink.Id((new Res(x.tag)).tag); return t;");
}

BEZEL_TEST(checkread_expression_walk_3971,
           new_ctor_argument_nested_inside_a_call_rejected_28_3) {
    expectMovedOutOnce(std::string(kConsumed) +
                       "var u := Sink.Take(new Res(x.tag)); return t + u;");
}

// ============================================================================
// NEGATIVE — a MARKER-FREE `if` expression.  The MARKED form already routed to
// `processMarkedArmConstruct`; the marker-free form fell off the end.
// ============================================================================

BEZEL_TEST(checkread_expression_walk_3971,
           conditional_then_arm_read_after_move_rejected_28_3) {
    expectMovedOutOnce(std::string(kConsumed) +
                       "var c := true; "
                       "var u := (if c then x.tag else 0); return t + u;");
}

BEZEL_TEST(checkread_expression_walk_3971,
           conditional_else_arm_read_after_move_rejected_28_3) {
    expectMovedOutOnce(std::string(kConsumed) +
                       "var c := true; "
                       "var u := (if c then 0 else x.tag); return t + u;");
}

BEZEL_TEST(checkread_expression_walk_3971,
           conditional_condition_read_after_move_rejected_28_3) {
    expectMovedOutOnce(std::string(kConsumed) +
                       "var u := (if x.tag > 0 then 1 else 0); return t + u;");
}

// ============================================================================
// NEGATIVE — a MARKER-FREE expression-position `case`: scrutinee, guard, arm.
// ============================================================================

BEZEL_TEST(checkread_expression_walk_3971,
           case_scrutinee_read_after_move_rejected_28_3) {
    expectMovedOutOnce(std::string(kConsumed) +
                       "var u := (case x.tag of 1 => 10, _ => 20); "
                       "return t + u;");
}

BEZEL_TEST(checkread_expression_walk_3971,
           case_arm_result_read_after_move_rejected_28_3) {
    expectMovedOutOnce(std::string(kConsumed) +
                       "var u := (case 1 of 1 => x.tag, _ => 20); "
                       "return t + u;");
}

BEZEL_TEST(checkread_expression_walk_3971,
           case_guard_read_after_move_rejected_28_3) {
    expectMovedOutOnce(std::string(kConsumed) +
                       "var u := (case 1 of var n when n = x.tag => 10, "
                       "_ => 20); return t + u;");
}

// ============================================================================
// NEGATIVE — the six remaining node kinds.
// ============================================================================

BEZEL_TEST(checkread_expression_walk_3971,
           collection_literal_element_read_after_move_rejected_28_3) {
    expectMovedOutOnce(std::string(kConsumed) +
                       "var a: [1]int := [x.tag]; return t + a[0];");
}

BEZEL_TEST(checkread_expression_walk_3971,
           interpolated_string_hole_read_after_move_rejected_28_3) {
    expectMovedOutOnce(std::string(kConsumed) +
                       "var s := $\"{x.tag}\"; return t + s.Length;");
}

BEZEL_TEST(checkread_expression_walk_3971,
           type_test_operand_read_after_move_rejected_28_3) {
    expectMovedOutOnce(std::string(kConsumed) +
                       "var b := x is Res; "
                       "return t + (if b then 1 else 0);");
}

BEZEL_TEST(checkread_expression_walk_3971,
           with_expression_override_read_after_move_rejected_28_3) {
    expectMovedOutOnce(std::string(kConsumed) +
                       "var p := new Pt(1, 2); var p2 := p with (A := x.tag); "
                       "return t + p2.A;");
}

BEZEL_TEST(checkread_expression_walk_3971,
           range_endpoint_read_after_move_rejected_28_3) {
    expectMovedOutOnce(std::string(kConsumed) +
                       "var r := x.tag..3; return t + r.Start!;");
}

BEZEL_TEST(checkread_expression_walk_3971,
           throw_operand_read_after_move_rejected_28_3) {
    expectMovedOutOnce(std::string(kConsumed) +
                       "var m: int? := null; "
                       "var u := m ?? throw Sink.MkEx(x.tag); return t + u;");
}

// ============================================================================
// NEGATIVE — the DISPOSE half.  §28.2's dominating-Dispose WARNING (not an
// error) has to reach the same newly-walked positions.  Three representatives,
// one per structural family (argument list, arm construct, literal).
// ============================================================================

BEZEL_TEST(checkread_expression_walk_3971,
           new_ctor_argument_read_after_dispose_warns_28_2) {
    OwnershipResult r = parseAndCheckOwnership(wrap(
        std::string(kDisposed) +
        "using var q := new Res(x.tag); return t + q.tag;"));
    BEZEL_EXPECT_TRUE(
        diagWithSeverity(r, bezel::Severity::Warning, kUseAfterDispose));
    BEZEL_EXPECT_TRUE(diagHasRealLocation(r, kUseAfterDispose));
    BEZEL_EXPECT_EQ(diagCount(r, kUseAfterDispose), 1);
}

BEZEL_TEST(checkread_expression_walk_3971,
           conditional_arm_read_after_dispose_warns_28_2) {
    OwnershipResult r = parseAndCheckOwnership(wrap(
        std::string(kDisposed) +
        "var c := true; var u := (if c then x.tag else 0); return t + u;"));
    BEZEL_EXPECT_TRUE(
        diagWithSeverity(r, bezel::Severity::Warning, kUseAfterDispose));
    BEZEL_EXPECT_TRUE(diagHasRealLocation(r, kUseAfterDispose));
    BEZEL_EXPECT_EQ(diagCount(r, kUseAfterDispose), 1);
}

BEZEL_TEST(checkread_expression_walk_3971,
           interpolated_string_hole_read_after_dispose_warns_28_2) {
    OwnershipResult r = parseAndCheckOwnership(wrap(
        std::string(kDisposed) +
        "var s := $\"{x.tag}\"; return t + s.Length;"));
    BEZEL_EXPECT_TRUE(
        diagWithSeverity(r, bezel::Severity::Warning, kUseAfterDispose));
    BEZEL_EXPECT_TRUE(diagHasRealLocation(r, kUseAfterDispose));
    BEZEL_EXPECT_EQ(diagCount(r, kUseAfterDispose), 1);
}

// ============================================================================
// AOT — the strict oracle's half of the same fact: the programs that used to
// compile and then heap-use-after-free must now not compile at all.
//
// `ran == false` alone would be AMBIGUOUS (issue 3892 / the
// check_ambiguous_ran_false ratchet): it is also false for a compiler crash, a
// runtime signal, a timeout, and a forced-ASan report — and a forced-ASan
// report is exactly what the PRE-fix program produces under the gate's
// `--full` oracle, so a bare `!ran` row here would pass WITHOUT the fix for
// entirely the wrong reason.  Pin the REJECTION — a real compile-time failure
// at a real location — and the §28.3 text.
// ============================================================================

BEZEL_TEST(checkread_expression_walk_3971,
           aot_new_ctor_argument_read_after_move_is_a_compile_error_28_3) {
    JitResult jr = runAot(wrap(std::string(kConsumed) +
                               "using var q := new Res(x.tag); "
                               "return t + q.tag;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kMovedOut));
}

BEZEL_TEST(checkread_expression_walk_3971,
           aot_conditional_arm_read_after_move_is_a_compile_error_28_3) {
    JitResult jr = runAot(wrap(std::string(kConsumed) +
                               "var c := true; "
                               "var u := (if c then x.tag else 0); "
                               "return t + u;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kMovedOut));
}

BEZEL_TEST(checkread_expression_walk_3971,
           aot_case_arm_read_after_move_is_a_compile_error_28_3) {
    JitResult jr = runAot(wrap(std::string(kConsumed) +
                               "var u := (case 1 of 1 => x.tag, _ => 20); "
                               "return t + u;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kMovedOut));
}

BEZEL_TEST(checkread_expression_walk_3971,
           aot_interpolation_hole_read_after_move_is_a_compile_error_28_3) {
    JitResult jr = runAot(wrap(std::string(kConsumed) +
                               "var s := $\"{x.tag}\"; return t + s.Length;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kMovedOut));
}

// ============================================================================
// NEGATIVE — the two JOIN state arms.  `checkRead`'s IdentifierExpr chain has
// arms for maybe-consumed (K7.4) and disposed-on-some-path as well as the
// straight-line MovedFrom/Disposed ones, and they were equally unreachable
// through the un-walked node kinds.  The `disposedOnSomePath` row is the
// sharpest of the whole file: pre-fix it compiled with ZERO ownership
// diagnostics and the produced program SEGV'd (exit 139).
// ============================================================================

BEZEL_TEST(checkread_expression_walk_3971,
           conditional_arm_read_after_branch_dispose_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(wrap(
        "var a := new Res(1); var c := true; "
        "if c then a.Dispose(); end; "
        "var u := (if c then a.tag else 0); return u;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(
        r, bezel::Severity::Error,
        "local `a` may have already been disposed on another branch"));
    BEZEL_EXPECT_TRUE(diagHasRealLocation(
        r, "local `a` may have already been disposed on another branch"));
}

BEZEL_TEST(checkread_expression_walk_3971,
           new_ctor_argument_read_after_branch_consume_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(wrap(
        "var a := new Res(1); var c := true; var t := 0; "
        "if c then t := Sink.Take(move a); end; "
        "using var q := new Res(a.tag); return t + q.tag;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(
        r, bezel::Severity::Error,
        "local `a` may have already been consumed on another branch"));
    BEZEL_EXPECT_TRUE(diagHasRealLocation(
        r, "local `a` may have already been consumed on another branch"));
}

BEZEL_TEST(checkread_expression_walk_3971,
           aot_conditional_arm_read_after_branch_dispose_is_a_compile_error) {
    JitResult jr = runAot(wrap(
        "var a := new Res(1); var c := true; "
        "if c then a.Dispose(); end; "
        "var u := (if c then a.tag else 0); return u;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        jr, "may have already been disposed on another branch"));
}

// ============================================================================
// GUARD — the #3941 preemption axis.  The two NEW rows were pinned by #3971
// to assert that `recordMovesIn`'s own consume-side path detects `move x`
// after a dominating `Dispose()`, with a verdict that the cascade suppression
// in `checkRead` would otherwise preempt.  Spec v0.303 / issue #4450 retires
// the original "precise §28.3 ERROR" claim (the ruling is the OTHER way: a
// consume under a dominating Dispose is §28.2's WARNING, not §28.3's error),
// but the GUARD invariant #3971 owns — that `recordMovesIn`'s arm is the
// one that reports, NOT a degraded generic warning — survives the ruling
// unchanged.  The guard now pins:
//   * `markMovedByName`'s Disposed arm produces the §28.2 consume-side
//     warning text (distinct from `checkRead`'s read-side wording), and
//   * `checkRead` does NOT also report a redundant §28.2 read-side warning
//     for the same `x` (one diagnostic per consume site, not two).
// This is the exact preemption `recordMovesIn` provides — it runs first
// here, so its OWN §28.2 warning wins — and it remains load-bearing for the
// reject-on-no-warning safety the original row traced.
// ============================================================================

BEZEL_TEST(checkread_expression_walk_3971,
           guard_new_move_argument_after_dispose_warns_28_2_4450) {
    OwnershipResult r = parseAndCheckOwnership(wrap(
        std::string(kDisposed) +
        "using var h := new Holder(move x); return t + h.v;"));
    // §28.2 WARNING (not §28.3 error) under the v0.303 ruling — the consume
    // side wording, distinct from the read side, with the `x` name and the
    // §28.2 citation all present.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(
        r, bezel::Severity::Warning,
        "move from a local `x` already disposed at this point (§28.2)"));
    // `checkRead` must NOT also fire on `move x` (no double warning) — this
    // is the preemption invariant #3971's GUARD originally owned.
    BEZEL_EXPECT_EQ(diagCount(r, kUseAfterDispose), 0);
}

BEZEL_TEST(checkread_expression_walk_3971,
           guard_new_bare_owned_argument_after_dispose_warns_28_2_4450) {
    OwnershipResult r = parseAndCheckOwnership(wrap(
        std::string(kDisposed) +
        "using var h := new Holder(x); return t + h.v;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(
        r, bezel::Severity::Warning,
        "move from a local `x` already disposed at this point (§28.2)"));
    BEZEL_EXPECT_EQ(diagCount(r, kUseAfterDispose), 0);
}

BEZEL_TEST(checkread_expression_walk_3971,
           guard_new_move_argument_after_move_reports_exactly_once_28_3) {
    expectMovedOutOnce(std::string(kConsumed) +
                       "using var h := new Holder(move x); return t + h.v;");
}

BEZEL_TEST(checkread_expression_walk_3971,
           guard_new_bare_owned_argument_after_move_reports_exactly_once) {
    expectMovedOutOnce(std::string(kConsumed) +
                       "using var h := new Holder(x); return t + h.v;");
}

// ============================================================================
// POSITIVE — the false-reject axis.  One legitimate program per newly walked
// node kind, all value-asserting through the AOT oracle.  Every one was
// measured byte-identical (rc, diagnostic count, runtime value) against the
// pre-fix compiler built from the same tree.
// ============================================================================

BEZEL_TEST(checkread_expression_walk_3971,
           positive_new_ctor_argument_reads_a_live_local) {
    JitResult jr = runAot(wrap(
        "using var a := new Res(5); "
        "using var q := new Res(a.tag + 1); "
        "return q.tag;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 6);
}

BEZEL_TEST(checkread_expression_walk_3971,
           positive_conditional_arms_read_live_locals) {
    JitResult jr = runAot(wrap(
        "using var a := new Res(5); using var b := new Res(7); "
        "var c := true; "
        "return (if c then a.tag else b.tag) + "
        "(if a.tag > 0 then 1 else 0);"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 6);
}

BEZEL_TEST(checkread_expression_walk_3971,
           positive_case_scrutinee_guard_and_arm_read_live_locals) {
    JitResult jr = runAot(wrap(
        "using var a := new Res(5); using var b := new Res(7); var n := 1; "
        "var p := (case n of 1 => a.tag, _ => b.tag); "
        "var q := (case a.tag of 5 => 1, _ => 0); "
        "var s := (case n of var m when m = a.tag => 1, _ => 2); "
        "return p + q + s;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 8);
}

BEZEL_TEST(checkread_expression_walk_3971,
           positive_collection_literal_reads_live_locals) {
    JitResult jr = runAot(wrap(
        "using var a := new Res(5); using var b := new Res(7); "
        "var arr: [2]int := [a.tag, b.tag]; "
        "return arr[0] + arr[1];"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 12);
}

BEZEL_TEST(checkread_expression_walk_3971,
           positive_interpolated_string_reads_a_live_local) {
    JitResult jr = runAot(wrap(
        "using var a := new Res(5); "
        "var s := $\"{a.tag}\"; "
        "return s.Length;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 1);
}

BEZEL_TEST(checkread_expression_walk_3971,
           positive_type_test_reads_a_live_local) {
    JitResult jr = runAot(wrap(
        "using var a := new Res(5); "
        "var isr := a is Res; "
        "return (if isr then a.tag else 0);"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 5);
}

BEZEL_TEST(checkread_expression_walk_3971,
           positive_with_expression_reads_a_live_local) {
    JitResult jr = runAot(wrap(
        "using var a := new Res(5); "
        "var p := new Pt(1, 2); var p2 := p with (A := a.tag); "
        "return p2.A + p2.B;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 7);
}

BEZEL_TEST(checkread_expression_walk_3971,
           positive_range_endpoint_reads_a_live_local) {
    JitResult jr = runAot(wrap(
        "using var a := new Res(5); "
        "var r := a.tag..7; "
        "var cnt := 0; "
        "for var i in a.tag..7 do cnt := cnt + 1; end; "
        "return r.Start! + cnt;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 8);
}

// The MARKED arm transfer must still work — `processMarkedArmConstruct`'s
// early return in `checkRead` is what keeps the marker-free arms below it from
// stealing the marked construct, and `markedConstructsProcessed_`'s dedup
// depends on that ordering.
BEZEL_TEST(checkread_expression_walk_3971,
           positive_marked_arm_transfer_still_consumes_exactly_once) {
    JitResult jr = runAot(wrap(
        "var a := new Res(5); var b := new Res(7); var c := true; "
        "using var r := (if c then move a else move b); "
        "return r.tag;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 5);
}

// The reads stay legal once the local is RE-ACQUIRED — §28.3 K1.3's
// "unreadable *until reassigned*".
BEZEL_TEST(checkread_expression_walk_3971,
           positive_newly_walked_positions_after_reacquisition) {
    JitResult jr = runAot(wrap(
        "var x := new Res(1); "
        "var t := Sink.Take(move x); "
        "x := new Res(6); "
        "using var q := new Res(x.tag); "
        "var c := true; "
        "var u := (if c then x.tag else 0); "
        "x.Dispose(); "
        "return t + q.tag + u;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 13);
}

// A local DEFINITELY ASSIGNED on every branch, then read in four of the newly
// walked positions at once.  The `Uninitialized` arm of `checkRead`'s
// IdentifierExpr chain is now reachable through those positions, so this pins
// that it does not fire where definite assignment holds.
BEZEL_TEST(checkread_expression_walk_3971,
           positive_definitely_assigned_local_read_in_new_positions) {
    JitResult jr = runAot(wrap(
        "var s: Res; var c := true; "
        "if c then s := new Res(11); else s := new Res(22); end; "
        "using var q := new Res(s.tag); "
        "var u := (if c then s.tag else 0); "
        "var lit: [1]int := [s.tag]; "
        "var st := $\"{s.tag}\"; "
        "s.Dispose(); "
        "return q.tag + u + lit[0] + st.Length;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 35);
}

// The newly walked positions inside a LOOP BODY — the shape #3931's
// deferred-poison fix made observable at all.  Four iterations, each minting
// and discharging its own value.
BEZEL_TEST(checkread_expression_walk_3971,
           positive_new_positions_inside_a_loop_body) {
    JitResult jr = runAot(wrap(
        "using var s := new Res(3); var n := 0; var i := 0; "
        "while i < 4 do "
        "  using var q := new Res(s.tag); "
        "  n := n + q.tag + (if s.tag > 0 then 1 else 0); "
        "  i := i + 1; "
        "end; "
        "return n;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 16);
}

// Consume and RE-ACQUIRE around a loop back edge, reading the re-acquired
// local in a `new` argument each iteration — §28.3 K1.3's "unreadable *until
// reassigned*" across the back edge K7.4 names.
BEZEL_TEST(checkread_expression_walk_3971,
           positive_reacquire_in_loop_then_read_in_a_new_argument) {
    JitResult jr = runAot(wrap(
        "var x := new Res(1); var n := 0; var i := 0; "
        "while i < 3 do "
        "  n := n + Sink.Take(move x); "
        "  x := new Res(i + 2); "
        "  n := n + (new Res(x.tag)).tag; "
        "  i := i + 1; "
        "end; "
        "x.Dispose(); "
        "return n;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 15);
}
