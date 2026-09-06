// Ownership — `MethodOwnership::recordMovesIn` is the ONE authority that
// validates and MARKS a `move` / owned-argument transfer for a LocalDecl-
// initializer or assignment-RHS binding context (`handleLocalDecl` /
// `handleAssignment` both call it, unconditionally, on the whole RHS). Before
// this fix its recursion covered exactly four node kinds — `UnaryExpr`
// (`move x`), `BinaryExpr`, `CallExpr`, `NewExpr` — and fell off the end for
// everything else with no fallback at all: a node kind outside that set was
// not walked, not diagnosed, not marked. `ConditionalExpr`, `CaseExpr` and
// `ThrowExpr` all sat in that unwalked remainder. Issues #3989 and #3990.
//
// This is the WRITE-side twin of #3971, which closed the identical shape of
// gap in `checkRead` (the READ-side authority) for nine node kinds. #3971's
// own new arms deliberately DECLINE to walk a top-level consuming operand
// (`isTopLevelConsumingForm`) precisely so `recordMovesIn` would process it —
// but `recordMovesIn` had no `ConditionalExpr`/`CaseExpr` arm to reach it
// with, so a `move` nested inside one of THOSE arms reached NEITHER function:
//
//   position                                  checkRead (post-#3971)   recordMovesIn (pre-fix)
//   -----------------------------------------  -----------------------  -----------------------
//   bare identifier / member / binary / call    walks (pre-#3971)        walks (UnaryExpr/BinaryExpr/CallExpr)
//   `new T(...)` ctor argument                  walks, defers moves      walks (NewExpr)
//   marker-free `if`/`case` arm (bare value)    walks, defers moves      DID NOT WALK AT ALL
//   marker-free `if`/`case` arm (nested move)   walks, defers moves      DID NOT WALK AT ALL
//   `throw` operand                             walks (read-only)        DID NOT WALK AT ALL
//   collection literal / interpolation / `is` /
//     `with` / range                            walks (pure reads)       n/a — none of these can carry
//                                                                        a `move`, so recordMovesIn never
//                                                                        needed an arm for them
//
// The MARKED arm-transfer form (`processMarkedArmConstruct` /
// `handleMarkedArmTransfer`, the `owned x` marker) is a THIRD, separate
// mechanism the LocalDecl/assignment callers invoke directly whenever
// `exprContainsMarkedTransfer` — unaffected by, and unaffected-by-design in,
// this fix (see the guard rows below).
//
// SPEC.  §28.3 K1.3 (v0.294): "the binding or slot is unreadable until
// reassigned, and discharge machinery skips it" — nothing carves out a
// `move` nested inside an `if`/`case`-expression arm. §28.3's consume table,
// row K3.4 (v0.294): "the marked arm transfer: `move x` as an if-expression
// branch or an expression-`case` arm result ... consuming on the path where
// its arm is selected" (§20, §19.2) — and K7.4: "a conditional kill of any
// row joins to maybe-consumed" — one clause, applying to every row, not only
// the bare-arm-value spelling. §31.1 "The throw operand" (v0.294): "`throw x`
// is a source-visible kill — the keyword itself is the announcement (K3.13),
// so the transfer marker is not written here". §17.12: "Valid in any
// expression position" for `throw expr`. §6755's `??` row (v0.294): "A
// `null`-literal or bottom-typed (`throw`) arm absorbs to the other arm's
// state (K7.5), legitimizing the `x ?? throw …` shape".
//
// MEASURED PRE-FIX (origin/main e240743 + #3971 = 4b597ce).
//
//   #3990 (§28.3 K1.3/K7/K7.4): `var x := new Res(1); x.Dispose(); using var
//   h := (if flag then new Holder(move x) else new Holder(new Res(0)));` —
//   rc=0, only an unrelated ctor auto-drop warning, and SEGVs
//   (`--sanitize=address`: SEGV on a null read inside `bz$M.Holder$ctor`,
//   `p`'s slot never having been populated because the disposed source was
//   silently "moved"). The direct, non-nested `new Holder(move x)` is
//   correctly rejected: "cannot move from disposed local `x` (§28.3)". The
//   `case`-expression twin is byte-identical.
//
//   #3989 (§31.1/§17.12): `var exc := new InvalidOperationException("boom");
//   ... r := n ?? throw exc; ...` (inside a try/catch) — rc=0, ONE spurious
//   diagnostic ("owned value in `exc` is implicitly disposed at the end of
//   its scope"), prints "throw: 77" (the catch ran), then heap-use-after-
//   frees (`--sanitize=address`: freed once by the exception machinery,
//   freed again by the wrongly-modeled scope-end auto-drop). The bare-
//   statement form `throw exc;` (no `??`) was always correctly discharged.
//
// THE FIX extends `recordMovesIn` (not a parallel walker) with three arms:
//
//   * A marker-free `ConditionalExpr`/`CaseExpr` (the MARKED form returns
//     immediately, `processMarkedArmConstruct` alone owns it — see the guard
//     rows): the condition/scrutinee is processed once, unconditionally;
//     each arm is then processed from an ISOLATED copy of the pre-construct
//     state (only one arm ever runs) and the per-arm exit states are folded
//     via `joinMarkerFreeArmEnds`, which reuses `joinInto` — the SAME §28.6
//     issue #338 / #1626 "Owned ⊓ MovedFrom ⇒ Owned+maybeConsumed" meet the
//     real CFG `if`-statement join already applies (Ownership.cpp ~3396) —
//     rather than a second join implementation. A `throw`/`null`-literal arm
//     is walked (for its own diagnostics) but excluded from the join (K7.5:
//     control never resumes after the construct on that path).
//   * `ThrowExpr`: delegates to `handleDischarge`'s existing ThrowExpr arm
//     (Ownership_Checks.cpp ~998), which already runs the §31.1 borrowed-
//     operand check, walks the operand's own owned-argument positions, and
//     discharges a bare named-owned-local operand — `handleDischarge` is
//     never otherwise reached for a ThrowExpr through a LocalDecl-
//     initializer / assignment-RHS, so this does not double-process.
//
// A REAL, PRE-EXISTING, MEASURED double-free was found (not introduced) by
// this fix and is fixed forward in the same commit: `own_captured_moved_
// from_3755`'s five "exclusive arms ... accepted" tests (plus two lit
// fixtures) asserted that a `move` in one marker-free arm and a lambda
// CAPTURE in the mutually-exclusive other arm compiled clean and ran safely.
// It does not — every existing row there only ever calls the non-consuming
// path; the CONSUMING path (never exercised) heap-use-after-frees on an
// unpatched tree exactly as #3990 does, for the identical structural reason.
// This fix's `joinMarkerFreeArmEnds` correctly classifies that binding
// Owned+maybeConsumed, and the resulting rejection is the SAME already-
// shipped §28.3 scope-end error the equivalent real STATEMENT form has
// always produced (see `maybe_consumed_join_matches_statement_form_
// rejected_28_3` below) — those five tests were updated (in this commit) to
// pin the correct, sound rejection instead. Filed as issue #3999 (the
// residual: this shape has no way to compile safely today — a real feature,
// not this fix's to build). A second, unrelated pre-existing double-free
// (`throw` on one branch of a plain `if`/`else` STATEMENT inside try/catch,
// no expression-position anything involved) was found while cross-checking
// #3989 against the statement form and is filed as issue #4000 — confirmed
// unrelated to `recordMovesIn` and NOT touched here.
//
// FALSE-REJECT COST: 0 measured beyond the #3999 finding above (which is a
// genuine, previously-silent double-free, not a false reject). Every
// `positive_*` row below is a legitimate program, value-asserted through the
// AOT oracle.
//
// per §28.3 K1.3/K7/K7.4, §31.1, §17.12, §20, §19.2, v0.294, #3971, #3989,
// #3990, #3999, #4000

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot, CodegenOptions — the AOT oracle
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::codegen::CodegenOptions;
using bezel::test::OwnershipResult;
using bezel::test::JitResult;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::hasAutoDropWarning;
using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

// `Res` is an ordinary owned class with a destructor (disposal is a real,
// ASan-visible event). `Holder` takes an owned `Res` and immediately
// discharges it in its own constructor, so none of the rows below carry the
// unrelated §28.3 K3.14 "owned parameter auto-drop" noise a bare `this.v :=
// r.tag;` body would (that warning is orthogonal to every rule this file
// tests and would otherwise have to be filtered out of every assertion).
const char* kPrelude =
    "module M; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  destructor end destructor; "
    "end Res; "
    "class Holder "
    "  public v: int; "
    "  public constructor(owned r: Res) this.v := r.tag; r.Dispose(); "
    "  end constructor; "
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

// `runAot` with generated-code ASan forced on for THIS row only, regardless
// of the suite-wide `BEZEL_AOT_FORCE_SANITIZE` env var: #3989's pre-fix
// symptom is a plain heap-use-after-free, which a non-ASan build can survive
// (allocator reuse luck) without ever crashing — so a bare `runAot` here
// would not reliably distinguish FAIL-WITHOUT from PASS-WITH. Forcing ASan
// makes the double-free deterministic: `r.ran` is FALSE (the process aborts)
// on the unpatched tree, TRUE (correct value) on the fixed one.
JitResult runAotAsan(const std::string& source) {
    CodegenOptions opts;
    opts.sanitizeAddress = true;
    return runAot(source, opts);
}

}  // namespace

// ============================================================================
// CONTROLS — spellings already correctly handled, anchoring the asymmetry.
// ============================================================================

// The direct, non-nested spelling: #3990's issue text's own contrast. Must
// stay rejected regardless of this fix.
BEZEL_TEST(record_moves_arm_and_throw_3989_3990,
           control_direct_move_of_disposed_local_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); x.Dispose(); "
        "using var h := new Holder(move x); return h.v;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasErrorMessage(r, "cannot move from disposed local `x` (\xc2\xa7"
                            "28.3)"));
}

// The bare-STATEMENT throw was always correctly discharged — #3989's issue
// text's own contrast for the expression form below.
BEZEL_TEST(record_moves_arm_and_throw_3989_3990,
           control_bare_throw_statement_discharges_cleanly_31_1) {
    JitResult r = runAotAsan(
        "module M; "
        "class Prog "
        "  public static method Main(): int "
        "    var exc := new InvalidOperationException(\"boom\"); "
        "    var r := 0; "
        "    try throw exc; catch var e: InvalidOperationException => "
        "      r := 77; , end try; "
        "    return r; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 77);
}

// ============================================================================
// NEGATIVE (#3990) — a `move` nested inside a marker-free if/case arm, of a
// DISPOSED local. FAIL-WITHOUT: rc=0, silent, SEGV. PASS-WITH: the precise
// §28.3 error, at a real location, and no program is ever produced.
// ============================================================================

BEZEL_TEST(record_moves_arm_and_throw_3989_3990,
           if_expr_move_of_disposed_local_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); x.Dispose(); var flag := true; "
        "using var h := (if flag then new Holder(move x) "
        "                        else new Holder(new Res(0))); "
        "return h.v;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasErrorMessage(r, "cannot move from disposed local `x` (\xc2\xa7"
                            "28.3)"));
}

BEZEL_TEST(record_moves_arm_and_throw_3989_3990,
           aot_if_expr_move_of_disposed_local_rejected_no_program_28_3) {
    JitResult r = runAot(wrap(
        "var x := new Res(1); x.Dispose(); var flag := true; "
        "using var h := (if flag then new Holder(move x) "
        "                        else new Holder(new Res(0))); "
        "return h.v;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

BEZEL_TEST(record_moves_arm_and_throw_3989_3990,
           case_expr_move_of_disposed_local_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); x.Dispose(); var n := 1; "
        "using var h := (case n of "
        "  1 => new Holder(move x), "
        "  _ => new Holder(new Res(0))); "
        "return h.v;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasErrorMessage(r, "cannot move from disposed local `x` (\xc2\xa7"
                            "28.3)"));
}

BEZEL_TEST(record_moves_arm_and_throw_3989_3990,
           aot_case_expr_move_of_disposed_local_rejected_no_program_28_3) {
    JitResult r = runAot(wrap(
        "var x := new Res(1); x.Dispose(); var n := 1; "
        "using var h := (case n of "
        "  1 => new Holder(move x), "
        "  _ => new Holder(new Res(0))); "
        "return h.v;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// A guard clause can carry the same nested move; the join logic must reach
// it too (§19.2's arm walk visits guard THEN body from the same isolated
// per-arm baseline). The guard compares a fresh `Holder` reference to `null`
// (always true — the guard's PASS/FAIL value is irrelevant to this test)
// rather than reading a member off it: a bare `MemberAccessExpr` target and
// a `CallExpr` argument are each their OWN, separate, still-open
// `recordMovesIn` gaps (issues #4003 and #3979 respectively) that would
// otherwise make this row accidentally exercise a DIFFERENT unfixed defect
// instead of the guard-walk this fix actually adds.
BEZEL_TEST(record_moves_arm_and_throw_3989_3990,
           case_expr_guard_move_of_disposed_local_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); x.Dispose(); var n := 1; "
        "var y := (case n of "
        "  var m when new Holder(move x) <> null => m, "
        "  _ => 0); "
        "return y;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasErrorMessage(r, "cannot move from disposed local `x` (\xc2\xa7"
                            "28.3)"));
}

// ============================================================================
// NEGATIVE (#3989) — `throw` as an expression (the RHS of `??`) must
// discharge its operand exactly like the statement form. FAIL-WITHOUT: rc=0,
// one spurious auto-drop warning, heap-use-after-free at runtime under
// forced ASan. PASS-WITH: zero diagnostics, runs clean to the correct value.
// ============================================================================

BEZEL_TEST(record_moves_arm_and_throw_3989_3990,
           throw_expr_in_null_coalesce_discharges_no_warning_31_1) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Prog "
        "  public static method Main(): int "
        "    var exc := new InvalidOperationException(\"boom\"); "
        "    var n: int? := null; var r := 0; "
        "    try r := n ?? throw exc; "
        "    catch var e: InvalidOperationException => r := 77; , end try; "
        "    return r; "
        "  end Main; "
        "end Prog;");
    // The whole point: no diagnostic AT ALL — `exc` is a real discharge, not
    // an implicitly-disposed plain local.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "exc"));
}

BEZEL_TEST(record_moves_arm_and_throw_3989_3990,
           aot_throw_expr_in_null_coalesce_runs_clean_no_double_free_31_1) {
    JitResult r = runAotAsan(
        "module M; "
        "class Prog "
        "  public static method Main(): int "
        "    var exc := new InvalidOperationException(\"boom\"); "
        "    var n: int? := null; var r := 0; "
        "    try r := n ?? throw exc; "
        "    catch var e: InvalidOperationException => r := 77; , end try; "
        "    return r; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 77);
}

// `throw` as a `case`-expression arm result (§19.2's own admitted shape),
// reached through the CaseExpr arm's walk rather than a BinaryExpr's — but
// on the NON-throwing arm only. (Selecting the throwing arm here hits a
// SEPARATE, confirmed pre-existing double-free — the exception-unwind sweep
// does not see a discharge that happened only inside an isolated, joined-
// away CaseExpr arm, exactly as issue #4000 already describes for an
// if/else STATEMENT branch; reproduces byte-identically on a tree with none
// of #3971/#3989/#3990 applied and is recorded as an addendum to #4000, not
// fixed here.) This row instead pins that the ThrowExpr arm's PRESENCE does
// not regress the ordinary, non-throwing path: `exc` is walked (for its own
// diagnostics, which correctly stay silent — nothing throws it) but,
// correctly excluded from the join as an absorbing arm, never taints the
// surviving arm's result.
BEZEL_TEST(record_moves_arm_and_throw_3989_3990,
           aot_throw_expr_as_case_arm_result_non_throwing_path_31_1) {
    JitResult r = runAotAsan(
        "module M; "
        "class Prog "
        "  public static method Main(): int "
        "    var exc := new InvalidOperationException(\"boom\"); "
        "    var n := 1; var r := 0; "
        "    r := (case n of 1 => 10, _ => throw exc); "
        "    return r; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ============================================================================
// GUARDS — the K3.4 MARKED arm-transfer form must be completely unaffected
// (`processMarkedArmConstruct` alone owns it; this fix's arms return
// immediately on `exprContainsMarkedTransfer`, never double-processing).
// ============================================================================

BEZEL_TEST(record_moves_arm_and_throw_3989_3990,
           guard_marked_arm_transfer_still_routes_unchanged_28_3) {
    JitResult r = runAotAsan(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Prog "
        "  public static method Main(): int "
        "    var a := new Res(1); var b := new Res(2); var c := true; "
        "    using var best := (if c then owned a else owned b); "
        "    return best.tag; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ============================================================================
// POSITIVE — the false-reject axis and the K7.4 join's correctness.
// ============================================================================

// Both arms move the SAME local: consumed on every non-absorbing arm, so the
// join is definite MovedFrom (not maybe-consumed) — no spurious scope-end
// diagnostic, and the program runs.
BEZEL_TEST(record_moves_arm_and_throw_3989_3990,
           positive_if_expr_move_in_every_arm_joins_moved_from_28_3) {
    JitResult r = runAotAsan(wrap(
        "var x := new Res(1); var flag := true; "
        "using var h := (if flag then new Holder(move x) "
        "                        else new Holder(move x)); "
        "return h.v;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// Neither arm touches the binding at all: no move recorded anywhere, no
// diagnostic, ordinary scope-end auto-drop only.
BEZEL_TEST(record_moves_arm_and_throw_3989_3990,
           positive_if_expr_neither_arm_touches_binding_28_3) {
    OwnershipResult r = parseAndCheckOwnership(wrap(
        "var x := new Res(9); var flag := true; "
        "using var h := (if flag then new Holder(new Res(1)) "
        "                        else new Holder(new Res(2))); "
        "x.Dispose(); return h.v;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A `throw`/absorbing arm alongside an ordinary borrow read of the SAME
// binding in the other arm: the throw arm's own diagnostics still run (its
// operand is walked), but K7.5 excludes it from the join, so the surviving
// (never-consuming) arm leaves the binding plain Owned — no spurious
// maybe-consumed classification from an arm that, per K7.5, never rejoins.
BEZEL_TEST(record_moves_arm_and_throw_3989_3990,
           positive_absorbing_throw_arm_does_not_poison_other_arm_28_3) {
    OwnershipResult r = parseAndCheckOwnership(wrap(
        "var x := new Res(3); "
        "var exc := new InvalidOperationException(\"e\"); "
        "var flag := true; "
        "var v := (if flag then x.tag else throw exc); "
        "x.Dispose(); return v;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ============================================================================
// MECHANISM — the K7.4 join for a LEGITIMATE (not pre-disposed) single-arm
// move is a REAL rejection (not a false reject): reaching the method's
// end-of-scope scan Owned+maybeConsumed is unsafe to auto-drop
// unconditionally, and this is the SAME diagnostic — proving the fix REUSES
// the existing authority rather than adding a second one — the equivalent
// real `if`/`else` STATEMENT already produces for the identical shape
// (`if flag then Sink.Take(move x); end;`, no else, no post-handling).
// ============================================================================

BEZEL_TEST(record_moves_arm_and_throw_3989_3990,
           maybe_consumed_join_matches_statement_form_rejected_28_3) {
    OwnershipResult exprForm = parseAndCheckOwnership(wrap(
        "var x := new Res(1); var flag := true; "
        "using var h := (if flag then new Holder(move x) "
        "                        else new Holder(new Res(0))); "
        "return h.v;"));
    OwnershipResult stmtForm = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Sink "
        "  public static method Take(owned r: Res): int return r.tag; "
        "  end Take; "
        "end Sink; "
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); var flag := true; var v := 0; "
        "    if flag then v := Sink.Take(move x); end; "
        "    return v; "
        "  end Main; "
        "end C;");
    const char* kNeedle =
        "owned value in `x` would be dropped here without disposal";
    BEZEL_EXPECT_TRUE(hasErrorMessage(exprForm, kNeedle));
    BEZEL_EXPECT_TRUE(hasErrorMessage(stmtForm, kNeedle));
}
