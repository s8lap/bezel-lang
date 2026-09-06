// Ownership — a COMPOUND assignment (`x += e`, `x -= e`, …) and the `x++` /
// `x--` statement received NO ownership handling of any kind, so a read of an
// already moved-from owned local through either form went completely
// undiagnosed and the produced program heap-use-after-freed.  The
// byte-identical read spelled as a plain assignment (`a := a + b`) is
// correctly rejected.  Issue #3969.
//
// SPEC — settled, not underspecified.
//
// §6.6 "Compound assignments" (v0.294), verbatim:
//
//   "For every operator above except `??=`, the statement `x op= e` is
//    equivalent to `x := x op e` with the target evaluated **once**:
//    - every constituent of the left-hand side's lvalue chain (grammar A.6
//      `LValue`) is evaluated exactly once, left to right: the base identifier
//      or `this`, each mid-chain member hop …
//    - the order is: the LHS chain's constituent expressions, then the final
//      segment's getter (where applicable), then `e`, then the operation, then
//      the store."
//
// §17.6 "The increment and decrement operators" (v0.294), verbatim:
//
//   "`x++` is equivalent to `x := x + 1` (and likewise `x--` to `x := x - 1`)
//    under exactly §6.6's rules"
//
// §28.3 "Local variables, ownership, and borrows", K1 "Value classification",
// item 3 (v0.294), verbatim:
//
//   "**moved-from** — the obligation has transferred out; the binding or slot
//    is unreadable until reassigned, and discharge machinery skips it"
//
// A compound assignment therefore READS its target and READS `e`; `x++` reads
// its operand.  Nothing in §28.3 carves either out.
//
// MEASURED PRE-FIX (parent e240743).  Every body's first lines are
// `var x := new Res(1); var t := Sink.Take(move x);` — `Take` disposes its
// `owned` parameter, so `x` is provably dangling once `Take` returns.
// `bezelc -o out prog.bzl` on each, then the same program under
// `--sanitize=address`:
//
//   | statement                          | rc | diagnostics | ASan          |
//   |------------------------------------|----|-------------|---------------|
//   | `t := t + x.tag;`   (plain assign) |  1 | §28.3 error | (rejected)    |
//   | `a += b;`   (class `operator +`)   |  0 | **NONE**    | heap-UAF      |
//   | `t += x.tag;`                      |  0 | **NONE**    | heap-UAF      |
//   | `x.tag += 1;`                      |  0 | **NONE**    | heap-UAF      |
//   | `x.tag++;`                         |  0 | **NONE**    | heap-UAF      |
//   | `t += Sink.Take(move x);`          |  0 | **NONE**    | heap-UAF      |
//
// The last row is a different mechanism from the first four: no read of a
// moved-from local is involved at all.  `handleAssignment` never reached
// `recordMovesIn` for a compound statement either, so the `move x` inside the
// RHS was never recorded, `x` stayed Owned, and §28.3's scope-end auto-drop
// disposed what `Take` had already freed — measured as "READ of size 8 …
// in main", freed in `bz$M.Sink$Take`, and as a bare SIGSEGV (rc=139) on the
// UNinstrumented build.
//
// Rows 3 and 4 (`x.tag += 1`, `x.tag++`) need no user-defined operator at
// all: they are ordinary `int` compound assignments whose LHS chain merely
// REACHES a moved-from class local, so the hole was far wider than the
// operator-overload spelling the issue's own repro exercises.
//
// ROOT CAUSE — two sites, one rule.  `MethodOwnership::handleAssignment`
// opened with `if (as.op != ":=") return;`, so the compound form fell out of
// the function before any of its work ran.  `MethodOwnership::visitStmt` had
// no `ast::IncrementStmt` arm at all — `x++` is not an `AssignmentStmt`, so
// relaxing the first site alone would not have reached it.
//
// THE FIX feeds the existing authority rather than adding a second one.  The
// compound arm read-checks its target and its RHS through `checkRead` — the
// ONE place that inspects tracked MovedFrom / Disposed / maybe-consumed /
// Uninitialized state — and runs `recordMovesIn` over the RHS, which §6.6
// makes unconditionally evaluated.  The `IncrementStmt` arm read-checks its
// operand through the same `checkRead`.  The RHS carve-outs are
// `isTopLevelConsumingForm` + `valueHasOwnedMarker`, the identical pair
// `handleLocalDecl` (#747) and the `:=` path (#3949) already ask.
//
// WHAT THE FIX DELIBERATELY DOES NOT DO.  Simply deleting the early return
// would have been wrong: ~60 lines below it the `:=` path does
// `st.state = producerState(*as.value)`, which for `a += b` would reclassify
// the TARGET `a` from Owned to the producer state of `b` (a bare borrow read)
// and silently discard `a`'s §28.3 disposal obligation — trading a
// use-after-free for a leak.  The correct producer for a compound assignment
// is the OPERATOR RESULT, not `as.value`; for the class-typed shape that
// result is owned, i.e. exactly the state the target already carries, so
// leaving the target's state untouched is the right answer and the displaced
// value is discharged by §28.7's drop-on-overwrite in codegen.  `??=` is
// excluded from `recordMovesIn` on §6.6's own words — it "is not a
// read-modify-write", its RHS runs only when the assignment occurs, and a
// conditionally-evaluated transfer is a K7.4 join, not an unconditional move.
//
// FALSE-REJECT COST: 0.  Differential over the whole tracked `.bzl` corpus —
// 2,978 files, of which 1,135 contain a compound-assignment or `++`/`--`
// token — compiled with `bezelc -c` by the pre-fix and post-fix compilers:
// byte-identical exit status and byte-identical diagnostic text on every
// single file (`md5sum` of the two 2,978-row transcripts is equal).  The same
// harness reports DIFF on the repro programs, so it is not blind.
//
// NO NEW LEAK, NO NEW DOUBLE-FREE.  Four loop-amplified programs at 500
// iterations under `--sanitize=address` with `detect_leaks=1`: an owned class
// LOCAL compound-assigned 500 times through `operator +` (drop-on-overwrite
// per iteration), an owned class FIELD compound-assigned 500 times, 500
// `b.n++` / `b.n += 2` on a `using var`, and the `move`-in-RHS shape.  The
// first three are clean and value-identical on BOTH compilers; the fourth is
// heap-use-after-free pre-fix and clean post-fix.
//
// per §6.6, §17.6, §28.3 K1.3, §28.7, v0.294, #3969

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

// How many diagnostics carry `needle` — one, never two: the new read-checks
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
    "  public static method operator +(a: Res, b: Res): owned Res "
    "    return new Res(a.tag + b.tag); end operator +; "
    "  destructor end destructor; "
    "end Res; "
    "class Sink "
    "  public static method Take(owned r: Res): int "
    "    var v := r.tag; r.Dispose(); return v; end Take; "
    "  public static method Peek(r: Res): int return r.tag; end Peek; "
    "end Sink; ";

std::string wrap(const std::string& body) {
    return std::string(kPrelude) +
           "class C "
           "  public static method Main(): int " +
           body +
           "  end Main; "
           "end C;";
}

// The shared opening of every use-after-MOVE row.  `Take` disposes its
// `owned` parameter, so `x` is dangling the moment `Take` returns.
const char* kConsumed =
    "var x := new Res(1); "
    "var t := Sink.Take(move x); ";

// The shared opening of every use-after-DISPOSE row.
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
// CONTROL — the plain-assignment spelling of the very same read, rejected
// since #3949.  It anchors the asymmetry every negative row below measures.
// ============================================================================

BEZEL_TEST(compound_assignment_read_after_consume_3969,
           control_plain_assignment_read_after_move_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        wrap(std::string(kConsumed) + "t := t + x.tag; return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
    BEZEL_EXPECT_EQ(diagCount(r, kMovedOut), 1);
}

// ============================================================================
// NEGATIVE — the compound-assignment TARGET is a read position (§6.6).
// FAIL WITHOUT THE FIX: zero diagnostics; the program heap-use-after-frees.
// ============================================================================

// The issue's own repro: a class-typed owned local compound-assigned through
// its `operator +` after being moved out.  The read is of the TARGET itself.
BEZEL_TEST(compound_assignment_read_after_consume_3969,
           compound_target_read_after_move_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        wrap(std::string(kConsumed) +
             "using var b := new Res(4); x += b; return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
    BEZEL_EXPECT_TRUE(diagHasRealLocation(r, kMovedOut));
    BEZEL_EXPECT_EQ(diagCount(r, kMovedOut), 1);
}

// The MEMBER-chain target — an ordinary `int` compound assignment whose LHS
// chain merely reaches the moved-from local.  No operator overload at all, so
// this exercises `checkRead`'s MemberAccessExpr arm over the assignment
// TARGET, a position nothing read-checked before.
BEZEL_TEST(compound_assignment_read_after_consume_3969,
           compound_member_target_read_after_move_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        wrap(std::string(kConsumed) + "x.tag += 1; return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
    BEZEL_EXPECT_TRUE(diagHasRealLocation(r, kMovedOut));
    BEZEL_EXPECT_EQ(diagCount(r, kMovedOut), 1);
}

// The `-=` twin — proves the arm keys on "not `:=`", never on `+=` alone.
BEZEL_TEST(compound_assignment_read_after_consume_3969,
           compound_minus_equals_target_read_after_move_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        wrap(std::string(kConsumed) + "x.tag -= 1; return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
    BEZEL_EXPECT_EQ(diagCount(r, kMovedOut), 1);
}

// ============================================================================
// NEGATIVE — the compound-assignment RHS is a read position (§6.6's `e`).
// ============================================================================

BEZEL_TEST(compound_assignment_read_after_consume_3969,
           compound_rhs_member_read_after_move_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        wrap(std::string(kConsumed) + "t += x.tag; return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
    BEZEL_EXPECT_TRUE(diagHasRealLocation(r, kMovedOut));
    BEZEL_EXPECT_EQ(diagCount(r, kMovedOut), 1);
}

BEZEL_TEST(compound_assignment_read_after_consume_3969,
           compound_rhs_call_argument_read_after_move_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        wrap(std::string(kConsumed) + "t += Sink.Peek(x); return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
    BEZEL_EXPECT_EQ(diagCount(r, kMovedOut), 1);
}

// ============================================================================
// NEGATIVE — `x++` / `x--` (`ast::IncrementStmt`), a DIFFERENT statement node
// that `visitStmt` had no arm for at all.  §17.6: "`x++` is equivalent to
// `x := x + 1` … under exactly §6.6's rules".
// ============================================================================

BEZEL_TEST(compound_assignment_read_after_consume_3969,
           increment_member_target_read_after_move_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        wrap(std::string(kConsumed) + "x.tag++; return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
    BEZEL_EXPECT_TRUE(diagHasRealLocation(r, kMovedOut));
    BEZEL_EXPECT_EQ(diagCount(r, kMovedOut), 1);
}

BEZEL_TEST(compound_assignment_read_after_consume_3969,
           decrement_member_target_read_after_move_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        wrap(std::string(kConsumed) + "x.tag--; return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
    BEZEL_EXPECT_EQ(diagCount(r, kMovedOut), 1);
}

// ============================================================================
// NEGATIVE — the AOT oracle's half of the same facts.
//
// `ran == false` alone would be AMBIGUOUS (issue 3892 / the
// check_ambiguous_ran_false ratchet): it is also false for a compiler crash,
// a runtime signal, a timeout, and a forced-ASan report — and a forced-ASan
// report is EXACTLY what each pre-fix program below produces under the gate's
// `--full` oracle, so a bare `!ran` row would pass WITHOUT the fix for
// entirely the wrong reason.  Pin the REJECTION — a real compile-time failure
// at a real location — and the §28.3 text.
// ============================================================================

BEZEL_TEST(compound_assignment_read_after_consume_3969,
           aot_compound_target_read_after_move_is_a_compile_error_28_3) {
    JitResult jr = runAot(
        wrap(std::string(kConsumed) +
             "using var b := new Res(4); x += b; return t;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kMovedOut));
}

BEZEL_TEST(compound_assignment_read_after_consume_3969,
           aot_compound_rhs_read_after_move_is_a_compile_error_28_3) {
    JitResult jr =
        runAot(wrap(std::string(kConsumed) + "t += x.tag; return t;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kMovedOut));
}

BEZEL_TEST(compound_assignment_read_after_consume_3969,
           aot_increment_read_after_move_is_a_compile_error_28_3) {
    JitResult jr = runAot(wrap(std::string(kConsumed) + "x.tag++; return t;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kMovedOut));
}

// ============================================================================
// NEGATIVE — the SECOND, independent half: the compound RHS's own transfers
// were never recorded, so the source stayed Owned and scope-end auto-drop
// disposed what the callee had already freed.
//
// Compile-time discriminator (no sanitizer needed): after
// `t += Sink.Take(move x);` the local `x` must be MOVED-FROM, so a later read
// of it is rejected.  PRE-FIX `x` was still Owned and the read was accepted.
// ============================================================================

BEZEL_TEST(compound_assignment_read_after_consume_3969,
           compound_rhs_transfer_leaves_source_moved_from_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        wrap("var x := new Res(1); "
             "var t := 0; "
             "t += Sink.Take(move x); "
             "return t + x.tag;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
    BEZEL_EXPECT_EQ(diagCount(r, kMovedOut), 1);
}

// The runtime half of the same fact, and a VALUE assertion rather than a
// rejection: with the transfer recorded, the scope-end auto-drop skips `x`
// and the program simply runs.  PRE-FIX this program compiled with zero
// diagnostics and then died — SIGSEGV (rc=139) on the plain build,
// "heap-use-after-free … READ of size 8 in main" under `--sanitize=address`
// — so `jr.ran` is false without the fix on BOTH oracles.
BEZEL_TEST(compound_assignment_read_after_consume_3969,
           aot_compound_rhs_transfer_does_not_double_dispose_28_3) {
    JitResult jr = runAot(
        wrap("var x := new Res(7); "
             "var t := 1; "
             "t += Sink.Take(move x); "
             "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 8);
}

// Loop-amplified twin of the row above — 100 consume-and-remint cycles, so a
// per-iteration double-dispose cannot hide behind a single-shot run.
BEZEL_TEST(compound_assignment_read_after_consume_3969,
           aot_compound_rhs_transfer_in_a_loop_does_not_double_dispose_28_3) {
    JitResult jr = runAot(
        wrap("var total := 0; "
             "var i := 0; "
             "while i < 100 do "
             "  var x := new Res(2); "
             "  total += Sink.Take(move x); "
             "  i += 1; "
             "end; "
             "return total;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 200);
}

// ============================================================================
// NEGATIVE — the DISPOSE twin.  §28.2's dominating-Dispose regime is a
// WARNING, not an error, and it was likewise silent at both statement forms.
// ============================================================================

BEZEL_TEST(compound_assignment_read_after_consume_3969,
           compound_rhs_read_after_dispose_warns_28_2) {
    OwnershipResult r = parseAndCheckOwnership(
        wrap(std::string(kDisposed) + "t += x.tag; return t;"));
    BEZEL_EXPECT_TRUE(
        diagWithSeverity(r, bezel::Severity::Warning, kUseAfterDispose));
    BEZEL_EXPECT_TRUE(diagHasRealLocation(r, kUseAfterDispose));
    BEZEL_EXPECT_EQ(diagCount(r, kUseAfterDispose), 1);
    // A warning, never an error — §28.2's straight-line use-after-dispose
    // regime, not §28.3's moved-from error.
    BEZEL_EXPECT_FALSE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
}

BEZEL_TEST(compound_assignment_read_after_consume_3969,
           increment_target_read_after_dispose_warns_28_2) {
    OwnershipResult r = parseAndCheckOwnership(
        wrap(std::string(kDisposed) + "x.tag++; return t;"));
    BEZEL_EXPECT_TRUE(
        diagWithSeverity(r, bezel::Severity::Warning, kUseAfterDispose));
    BEZEL_EXPECT_EQ(diagCount(r, kUseAfterDispose), 1);
}

// ============================================================================
// POSITIVE — the false-reject axis.  Compound assignment and `++` are
// everywhere in real code; over-rejecting them is worse than the bug.  Every
// row asserts a VALUE, and every one was measured byte-identical (exit status
// and diagnostics) against the pre-fix compiler.
// ============================================================================

// The single commonest shape in the language: `i += 1` / `i++` on live ints,
// inside a loop so the dataflow fixpoint sees the statements repeatedly.
BEZEL_TEST(compound_assignment_read_after_consume_3969,
           positive_int_accumulation_in_a_loop_6_6) {
    JitResult jr = runAot(wrap(
        "var total := 0; "
        "var i := 0; "
        "while i < 5 do total += i; i++; end; "
        "total *= 2; "
        "total -= 3; "
        "return total;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 17);
}

// A LIVE owned class local compound-assigned through its `operator +`: the
// target keeps its Owned state (the fix makes no producer-state transition),
// §28.7 drop-on-overwrite discharges the displaced value, and the explicit
// `Dispose()` discharges the final one.
BEZEL_TEST(compound_assignment_read_after_consume_3969,
           positive_live_owned_local_compound_assignment_28_7) {
    JitResult jr = runAot(wrap(
        "var a := new Res(3); "
        "using var b := new Res(4); "
        "a += b; "
        "var r := a.tag; "
        "a.Dispose(); "
        "return r;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 7);
}

// The same, amplified — 50 drop-on-overwrite cycles.  A per-iteration leak or
// double-free would not survive this row under the gate's `--full` oracle.
BEZEL_TEST(compound_assignment_read_after_consume_3969,
           positive_live_owned_local_compound_assignment_amplified_28_7) {
    JitResult jr = runAot(wrap(
        "var a := new Res(0); "
        "using var one := new Res(1); "
        "var i := 0; "
        "while i < 50 do a += one; i++; end; "
        "var r := a.tag; "
        "a.Dispose(); "
        "return r;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 50);
}

// A read AFTER the local has been re-acquired: the moved-from state is gone,
// so both statement forms are legal again (§28.3 K1.3 — "unreadable *until
// reassigned*").
BEZEL_TEST(compound_assignment_read_after_consume_3969,
           positive_reacquired_local_compound_and_increment_28_3) {
    JitResult jr = runAot(wrap(
        "var x := new Res(1); "
        "var t := Sink.Take(move x); "
        "x := new Res(6); "
        "t += x.tag; "
        "x.tag++; "
        "t += x.tag; "
        "x.Dispose(); "
        "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 14);
}

// A BORROW target and a borrow RHS — no ownership state involved at all.
BEZEL_TEST(compound_assignment_read_after_consume_3969,
           positive_borrow_target_and_rhs_6_6) {
    JitResult jr = runAot(wrap(
        "using var src := new Res(4); "
        "var b := src; "
        "var t := 1; "
        "t += Sink.Peek(b); "
        "b.tag++; "
        "t += b.tag; "
        "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 10);
}

// `??=` — §6.6 excludes it from the read-modify-write desugaring, so the fix
// read-checks it but never records a move through it.  A legitimate `??=`
// must still compile and behave.
BEZEL_TEST(compound_assignment_read_after_consume_3969,
           positive_null_coalescing_assignment_7_7) {
    JitResult jr = runAot(wrap(
        "var n: int? := null; "
        "n ??= 5; "
        "var m: int? := 3; "
        "m ??= 9; "
        "return n! + m!;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 8);
}

// A string `+=` concat and an owned-FIELD `int` compound through implicit
// `this` — the two commonest non-local compound targets.
BEZEL_TEST(compound_assignment_read_after_consume_3969,
           positive_string_concat_and_field_compound_4_6) {
    JitResult jr = runAot(
        "module M; "
        "class Bag "
        "  public total: int; "
        "  public label: string; "
        "  public constructor() this.total := 0; this.label := \"\"; "
        "  end constructor; "
        "  public method Add(k: int) "
        "    this.total += k; this.label += \"x\"; this.total++; "
        "  end Add; "
        "  destructor end destructor; "
        "end Bag; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Bag(); "
        "    b.Add(3); "
        "    b.Add(4); "
        "    return b.total + b.label.Length; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 11);
}

// An INDEXED target and an indexed RHS — `checkRead`'s IndexExpr arm walks
// the index sub-expressions, so a live index must not be flagged.
BEZEL_TEST(compound_assignment_read_after_consume_3969,
           positive_indexed_target_and_rhs_6_6) {
    JitResult jr = runAot(wrap(
        "var a := new Array<int>(4, 0); "
        "var i := 0; "
        "while i < 4 do a[i] := i; i++; end; "
        "a[2] += 5; "
        "a[3]++; "
        "var t := a[2] + a[3]; "
        "a.Dispose(); "
        "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 11);
}
