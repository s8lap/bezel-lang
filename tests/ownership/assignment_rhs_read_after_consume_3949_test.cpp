// Ownership — an ASSIGNMENT RHS was the one read position that never reached
// `checkRead`, so a read of an already moved-from (or explicitly disposed)
// owned local in an assignment RHS went completely undiagnosed, in plain
// STRAIGHT-LINE code with no loop involved.  The byte-identical read spelled
// as a bare statement, a `var` initializer or a `return` operand was correctly
// rejected.  Issue #3949.
//
// SPEC — settled, not underspecified.  §28.3 "Local variables, ownership, and
// borrows", K1 "Value classification", item 3 (v0.294), verbatim:
//
//   "**moved-from** — the obligation has transferred out; the binding or slot
//    is unreadable until reassigned, and discharge machinery skips it"
//
// and the same list's auxiliary states:
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
// An assignment RHS is a read position like any other; nothing in §28.3
// carves one out.
//
// MEASURED PRE-FIX (parent b733d40 = origin/main 2760e31 + #3864 + #3931 +
// #3950).  Every body begins `var x := new Res(1); var t := Sink.Take(move x);`
// — `Take` never discharges its `owned` parameter, so §28.3's auto-drop frees
// the moved-in value at the end of `Take` and `x` is provably dangling once
// `Take` returns:
//
//   | third line                       | rc | diagnostics |
//   |----------------------------------|----|-------------|
//   | `Sink.Peek(x);`      (bare stmt) |  1 | §28.3 moved-out error |
//   | `t := Sink.Peek(x);`             |  0 | **NONE** |
//   | `t := t + Sink.Peek(x);`         |  0 | **NONE** |
//   | `var u := Sink.Peek(x);`         |  1 | §28.3 moved-out error |
//   | `return t + x.tag;`              |  1 | §28.3 moved-out error |
//   | `t := x.tag;`                    |  0 | **NONE** |
//
// The only difference between a rejection and silent acceptance was whether
// the read sat in an assignment RHS.  All three accepted programs
// heap-use-after-free: ASan reported "heap-use-after-free" with the free
// frame in `bz$M.Sink$Take`.  The DISPOSE twin behaved identically — §28.2's
// "use of `x` after a dominating explicit Dispose() call" fired for the
// bare-statement and `var` spellings and was silent for `t := Sink.Peek(x);`
// and `t := x.tag;`.
//
// ROOT CAUSE: `handleLocalDecl` opens with `checkRead(*ld.initializer, false)`;
// `handleReturn` read-checks its operand; `handleDischarge` read-checks a
// bare-statement call's arguments; `visitStmt`'s `LocalConstDecl` arm got its
// `checkRead` in #3864.  `handleAssignment` had `checkRead` calls ONLY for an
// `IndexExpr` assignment TARGET (the #1928 / #1932 move-only-inline-array
// gates) — never for `as.value`.  Its other RHS work (`recordMovesIn`,
// `checkBorrowStoreEscape`, `processMarkedArmConstruct`,
// `checkBareBorrowTempLaunder`) each answers a different question; none
// inspects the tracked state of a bare identifier read inside the RHS.
//
// THE FIX feeds the existing authority rather than adding a second one:
// `checkRead(*as.value, /*selfTarget=*/false)` in `handleAssignment`, before
// `recordMovesIn` so the state observed is the one at the top of the
// statement — the same order `handleLocalDecl` uses.  Two shapes stay
// excluded, exactly as `handleLocalDecl` has excluded them since #747: a
// top-level consuming form (`y := move x;` / `y := await x;`) and the
// assignment-position `owned` marker (`y := owned x;`), whose own, more
// specific checks own the diagnostic.  That exclusion predicate was an inline
// two-line test inside `handleLocalDecl`; it is now the named
// `isTopLevelConsumingForm` and BOTH sites ask it, so the decision has one
// spelling rather than two that can drift.
//
// FALSE-REJECT COST: 0.  Nine legitimate assignment-RHS shapes (a read of a
// borrow; a read after re-acquisition; a plain-int accumulation; `x := move y`;
// `x := owned y`; a move into an `owned` field; an owned-returning call in a
// loop; an if-expression RHS; a two-cycle consume/re-acquire) are byte-
// identical pre and post — same rc, same diagnostic count, same runtime value
// — and are pinned below as value-asserting positives.
//
// per §28.3 K1.3, §28.2, v0.294, #3949

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
    "  destructor end destructor; "
    "end Res; "
    "class Sink "
    "  public static method Take(owned r: Res): int return r.tag; end Take; "
    "  public static method Peek(r: Res): int return r.tag; end Peek; "
    "  public static method Give(): owned Res return new Res(3); end Give; "
    "end Sink; "
    "class Holder "
    "  public owned f: Res?; "
    "  public constructor() end constructor; "
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
// value at the end of `Take`.
const char* kConsumed =
    "var x := new Res(1); "
    "var t := Sink.Take(move x); ";

// The shared first two lines of every use-after-DISPOSE row.
const char* kDisposed =
    "var x := new Res(1); "
    "var t := 0; "
    "x.Dispose(); ";

const char* kMovedOut =
    "owned value in `x` was already moved out at this point (§28.3)";
const char* kUseAfterDispose =
    "use of `x` after a dominating explicit Dispose() call";

}  // namespace

// ============================================================================
// CONTROLS — the three spellings that were ALREADY rejected before this fix.
// They anchor the asymmetry every negative row below measures, and they must
// keep reporting exactly once.
// ============================================================================

BEZEL_TEST(assignment_rhs_read_after_consume_3949,
           control_bare_statement_read_after_move_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        wrap(std::string(kConsumed) + "Sink.Peek(x); return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
    BEZEL_EXPECT_EQ(diagCount(r, kMovedOut), 1);
}

BEZEL_TEST(assignment_rhs_read_after_consume_3949,
           control_var_initializer_read_after_move_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        wrap(std::string(kConsumed) + "var u := Sink.Peek(x); return t + u;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
    BEZEL_EXPECT_EQ(diagCount(r, kMovedOut), 1);
}

BEZEL_TEST(assignment_rhs_read_after_consume_3949,
           control_return_operand_read_after_move_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        wrap(std::string(kConsumed) + "return t + x.tag;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
    BEZEL_EXPECT_EQ(diagCount(r, kMovedOut), 1);
}

// ============================================================================
// NEGATIVE — the three assignment-RHS spellings that were silently accepted.
// FAIL WITHOUT THE FIX: zero diagnostics, and the produced program
// heap-use-after-frees.
// ============================================================================

// The plain call-in-RHS spelling.
BEZEL_TEST(assignment_rhs_read_after_consume_3949,
           assignment_rhs_call_argument_read_after_move_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        wrap(std::string(kConsumed) + "t := Sink.Peek(x); return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
    BEZEL_EXPECT_TRUE(diagHasRealLocation(r, kMovedOut));
    BEZEL_EXPECT_EQ(diagCount(r, kMovedOut), 1);
}

// Nested one level deeper, inside a binary operand — `checkRead`'s BinaryExpr
// recursion has to carry the check down.
BEZEL_TEST(assignment_rhs_read_after_consume_3949,
           assignment_rhs_binary_operand_read_after_move_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        wrap(std::string(kConsumed) + "t := t + Sink.Peek(x); return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
    BEZEL_EXPECT_TRUE(diagHasRealLocation(r, kMovedOut));
    BEZEL_EXPECT_EQ(diagCount(r, kMovedOut), 1);
}

// The bare MEMBER read — no call involved at all, so it exercises
// `checkRead`'s MemberAccessExpr arm rather than its CallExpr arm.
BEZEL_TEST(assignment_rhs_read_after_consume_3949,
           assignment_rhs_member_read_after_move_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        wrap(std::string(kConsumed) + "t := x.tag; return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
    BEZEL_EXPECT_TRUE(diagHasRealLocation(r, kMovedOut));
    BEZEL_EXPECT_EQ(diagCount(r, kMovedOut), 1);
}

// The AOT oracle's half of the same fact: the program that used to compile and
// then heap-use-after-free must now not compile at all.
//
// `ran == false` alone would be AMBIGUOUS (issue 3892 / the
// check_ambiguous_ran_false ratchet): it is also false for a compiler crash, a
// runtime signal, a timeout, and a forced-ASan report — and a forced-ASan
// report is exactly what the PRE-fix program produces under the gate's
// `--full` oracle, so a bare `!ran` row here would pass WITHOUT the fix for
// entirely the wrong reason.  Pin the REJECTION — a real compile-time failure
// at a real location — and the §28.3 text.
BEZEL_TEST(assignment_rhs_read_after_consume_3949,
           aot_assignment_rhs_read_after_move_is_a_compile_error_28_3) {
    JitResult jr = runAot(
        wrap(std::string(kConsumed) + "t := Sink.Peek(x); return t;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kMovedOut));
}

// ============================================================================
// NEGATIVE — the DISPOSE half.  §28.2's dominating-Dispose warning (not an
// error) was likewise silent in an assignment RHS.
// ============================================================================

BEZEL_TEST(assignment_rhs_read_after_consume_3949,
           control_bare_statement_read_after_dispose_warns_28_2) {
    OwnershipResult r = parseAndCheckOwnership(
        wrap(std::string(kDisposed) + "Sink.Peek(x); return t;"));
    BEZEL_EXPECT_TRUE(
        diagWithSeverity(r, bezel::Severity::Warning, kUseAfterDispose));
}

BEZEL_TEST(assignment_rhs_read_after_consume_3949,
           assignment_rhs_call_argument_read_after_dispose_warns_28_2) {
    OwnershipResult r = parseAndCheckOwnership(
        wrap(std::string(kDisposed) + "t := Sink.Peek(x); return t;"));
    BEZEL_EXPECT_TRUE(
        diagWithSeverity(r, bezel::Severity::Warning, kUseAfterDispose));
    BEZEL_EXPECT_TRUE(diagHasRealLocation(r, kUseAfterDispose));
    BEZEL_EXPECT_EQ(diagCount(r, kUseAfterDispose), 1);
    // A warning, never an error — §28.2's straight-line use-after-dispose
    // regime, not §28.3's moved-from error.
    BEZEL_EXPECT_FALSE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
}

BEZEL_TEST(assignment_rhs_read_after_consume_3949,
           assignment_rhs_member_read_after_dispose_warns_28_2) {
    OwnershipResult r = parseAndCheckOwnership(
        wrap(std::string(kDisposed) + "t := x.tag; return t;"));
    BEZEL_EXPECT_TRUE(
        diagWithSeverity(r, bezel::Severity::Warning, kUseAfterDispose));
    BEZEL_EXPECT_EQ(diagCount(r, kUseAfterDispose), 1);
}

// ============================================================================
// POSITIVE — the false-reject axis.  Every one of these is a legitimate
// assignment RHS that the new read-check must leave alone.  All nine were
// measured byte-identical (rc, diagnostic count, runtime value) against the
// pre-fix compiler.
// ============================================================================

// A read of a BORROW — the commonest assignment RHS in the language.
BEZEL_TEST(assignment_rhs_read_after_consume_3949,
           positive_rhs_reads_a_borrow_28_3) {
    JitResult jr = runAot(wrap(
        "using var src := new Res(4); "
        "var b := src; "
        "var t := 0; "
        "t := Sink.Peek(b); "
        "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 4);
}

// A read AFTER the local has been re-acquired: the moved-from state is gone,
// so the read is legal again (§28.3 K1.3 — "unreadable *until reassigned*").
BEZEL_TEST(assignment_rhs_read_after_consume_3949,
           positive_rhs_reads_a_reacquired_local_28_3) {
    JitResult jr = runAot(wrap(
        "var x := new Res(1); "
        "var t := Sink.Take(move x); "
        "x := new Res(6); "
        "t := t + Sink.Peek(x); "
        "x.Dispose(); "
        "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 7);  // 1 + 6
}

// `x := move y` — a TOP-LEVEL consuming form.  It must NOT be read-checked:
// the more specific move check owns the diagnostic there.
BEZEL_TEST(assignment_rhs_read_after_consume_3949,
           positive_rhs_is_a_top_level_move_28_3_K3_4) {
    JitResult jr = runAot(wrap(
        "var x := new Res(1); "
        "var y := new Res(2); "
        "var t := x.tag; "
        "x := move y; "
        "t := t + x.tag; "
        "x.Dispose(); "
        "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 3);  // 1 + 2
}

// `x := owned y` — the assignment-position transfer marker (v0.180, #1965).
// Not visible in the expression, so its exclusion is tested on the flag.
BEZEL_TEST(assignment_rhs_read_after_consume_3949,
           positive_rhs_carries_the_owned_marker_28_3) {
    JitResult jr = runAot(wrap(
        "var x := new Res(1); "
        "var y := new Res(2); "
        "var t := 0; "
        "x.Dispose(); "
        "x := owned y; "
        "t := x.tag; "
        "x.Dispose(); "
        "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 2);
}

// A move INTO an owned field, then a read back through the field — the field
// is a storage read (a borrow of its holder), not a tracked local.
BEZEL_TEST(assignment_rhs_read_after_consume_3949,
           positive_move_into_an_owned_field_then_read_it_back_28_5) {
    JitResult jr = runAot(wrap(
        "var h := new Holder(); "
        "var x := new Res(7); "
        "h.f := move x; "
        "var t := 0; "
        "if h.f <> null then t := h.f!.tag; end; "
        "h.Dispose(); "
        "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 7);
}

// A loop whose body re-mints an owned local every iteration.  This is the
// shape #2436 hardened `checkRead` for: a STALE prior-iteration state carried
// around the back edge must not produce a spurious rejection now that the
// assignment RHS is read-checked too.
BEZEL_TEST(assignment_rhs_read_after_consume_3949,
           positive_owned_call_result_read_in_a_loop_28_3) {
    JitResult jr = runAot(wrap(
        "var t := 0; "
        "var i := 0; "
        "while i < 3 do "
        "  var x := Sink.Give(); "
        "  t := t + Sink.Peek(x); "
        "  x.Dispose(); "
        "  i := i + 1; "
        "end; "
        "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 9);  // 3 + 3 + 3
}

// An if-EXPRESSION assignment RHS reading two live `using var` locals —
// `checkRead` routes marked arm constructs to `processMarkedArmConstruct`, so
// a marker-FREE construct must simply pass through.
BEZEL_TEST(assignment_rhs_read_after_consume_3949,
           positive_if_expression_rhs_over_live_locals_20) {
    JitResult jr = runAot(wrap(
        "using var a := new Res(2); "
        "using var b := new Res(3); "
        "var t := 0; "
        "t := (if a.tag > b.tag then a.tag else b.tag); "
        "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 3);
}

// Two consume/re-acquire cycles: the RHS reads the local in a `move` position
// each time, and the local is live again at every read.
BEZEL_TEST(assignment_rhs_read_after_consume_3949,
           positive_two_consume_reacquire_cycles_28_7) {
    JitResult jr = runAot(wrap(
        "var x := new Res(1); "
        "var t := 0; "
        "t := Sink.Take(move x); "
        "x := new Res(9); "
        "t := t + Sink.Take(move x); "
        "x := new Res(2); "
        "x.Dispose(); "
        "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 10);  // 1 + 9
}

// A plain-int accumulation — the overwhelmingly common assignment, over
// locals the ownership pass does not track at all.
BEZEL_TEST(assignment_rhs_read_after_consume_3949,
           positive_untracked_value_local_accumulation) {
    JitResult jr = runAot(wrap(
        "var acc := 0; "
        "var i := 0; "
        "while i < 4 do "
        "  acc := acc + i; "
        "  i := i + 1; "
        "end; "
        "return acc;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 6);  // 0 + 1 + 2 + 3
}
