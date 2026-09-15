// Spec-derived regression tests for issue #4139 — `caseHasUnguardedCatchAllArm`
// (added by fix #4116) was a purely SYNTACTIC catch-all detector (an
// unguarded `_` or untyped `var x` arm), so it did not recognize the sibling
// shape §19.6 also grants exhaustiveness to: a `case` the compiler proves
// exhaustive by CLOSED-VALUE-SPACE analysis (every enum constant, every
// choice case, a fully-covered `closed`/`sealed` hierarchy leaf set) with NO
// wildcard arm. A `case` of that shape, where every arm consumes the same
// outer owned local, was FALSELY REJECTED on a subsequent reassignment,
// dispose, or read of that local — the direct closed-value-space analogue of
// #4116's own wildcard-carve-out control.
//
// ROOT CAUSE (confirmed by reading `MethodOwnership::visitStmt`'s CaseStmt
// arm and `applyUsingVarBorrowPinInLambdaStmt`'s CaseStmt arm, both
// Ownership.cpp): both join sites fold the pre-case snapshot into the arm
// join as a stand-in for §19.6's no-match fallthrough UNLESS the no-match
// edge is provably dead. Through v0.295 "provably dead" was decided by
// `caseHasUnguardedCatchAllArm`, a four-line syntactic re-derivation that
// covered only the wildcard disjunct of §19.6's exhaustiveness rule, not the
// closed-value-space disjunct `sema::ExprTC::caseArmsExhaustive`
// (ExprTypecheck.cpp) already proves for the expression-position
// non-exhaustiveness warning.
//
// FIX (§19.6, v0.298): cache `caseArmsExhaustive`'s verdict on the AST as
// `ast::CaseStmt::noMatchEdgeStaticallyDead` (Node.hpp), set unconditionally
// by `ExprTC::visitStmt`'s CaseStmt arm (ExprTypecheck.cpp) — WITHOUT
// `CaseStmt::totalityGranted`'s additional "every arm diverges" conjunction,
// since that conjunction is irrelevant to this join (it matters only for
// §21.7's always-returns check). Both ownership join sites now read the
// cached field directly instead of re-deriving a narrower syntactic proxy —
// this removes `caseHasUnguardedCatchAllArm` entirely rather than keeping it
// alongside a second, drifting spelling of the same §19.6 decision (per the
// LANE_RULES duplicated-classification-predicate rule).
//
// MEASURED on this batch's base (a3da0a6, reconfigured, freshly-built
// `build/bezelc`): the issue's own enum repro (mirrored in Row 1 below)
// compiles with `cannot reassign \`x\`: it may have already been consumed on
// another branch (maybe-consumed join, §28.3 K1.4)` and a matching "cannot
// dispose" diagnostic; PASS-WITH: rc=0, clean.
//
// FALSE-ACCEPT GRID (§15 — the point of this fix, since it REMOVES
// rejections): Rows 2-4 and 7 are REJECT-side controls proving the widened
// carve-out does not also swallow a genuinely non-exhaustive `case` — a
// missing enum arm (Row 2), a catch-all present but GUARDED (not exhaustive
// per §19.6's "an arm carrying a `when` guard contributes nothing", Row 3),
// a `closed` hierarchy with an unhandled leaf (Row 4), and a `case` over an
// OPEN (non-sealed/non-closed) hierarchy where exhaustiveness cannot be
// proven at all (Row 7) all still reject identically before and after this
// fix.
//
// per §19.6, §28.3 K1.4, v0.298, #4139, #4116

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot / runAotChecked
#include "support/Diagnostic.hpp"
#include "support/ownership_support.hpp"   // parseAndCheckOwnership, hasErrorMessage
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::hasErrorMessage;
using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// A destructor-counter Res, matching the established family idiom (fix
// #4116's own kRes: case_stmt_maybe_consumed_join_4116_test.cpp).
const char* kRes =
    "module M; "
    "class Res "
    "  public static live: int := 0; "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; Res.live := Res.live + 1; end constructor; "
    "  destructor Res.live := Res.live - 1; end destructor; "
    "end Res; "
    "class Sink "
    "  public static method Take(owned r: Res): int return r.tag; end Take; "
    "  public static method Peek(r: Res): int return r.tag; end Peek; "
    "end Sink; ";

const char* kColorEnum =
    "enum Color "
    "  Red, Green, Blue; "
    "end Color; ";

const char* kAnimalHierarchy =
    "closed class Animal "
    "end Animal; "
    "sealed class Dog: Animal "
    "end Dog; "
    "sealed class Cat: Animal "
    "end Cat; ";

const std::string kReassignExpectedMsg =
    "cannot reassign `x`: it may have already been consumed on another "
    "branch (maybe-consumed join, §28.3 K1.4)";

}  // namespace

// ===========================================================================
// Cell A: enum, exhaustive, NO wildcard arm -- MUST ACCEPT (the bug).
// FAIL-WITHOUT: baseline (a3da0a6) rejects with kReassignExpectedMsg
// (measured). PASS-WITH: hasErrors() is false.
// ===========================================================================
BEZEL_TEST(case_stmt_exhaustive_no_wildcard_join_4139,
           enum_exhaustive_no_wildcard_every_arm_consumes_reassign_accepted) {
    std::string src =
        std::string(kRes) + kColorEnum +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var t := 0; "
        "    var n := Color.Red; "
        "    case n of "
        "      Color.Red => do t := t + Sink.Take(move x); end, "
        "      Color.Green => do t := t + Sink.Take(move x); end, "
        "      Color.Blue => do t := t + Sink.Take(move x); end "
        "    end case; "
        "    x := new Res(9); "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// Cell B (reject-side control, §15): an enum case MISSING an arm (`Blue`
// uncovered) -- no wildcard, not exhaustive -- must still reject. Unaffected
// by this fix on both sides (the no-match edge is genuinely live here).
// ===========================================================================
BEZEL_TEST(case_stmt_exhaustive_no_wildcard_join_4139,
           enum_missing_arm_not_exhaustive_reassign_still_rejected) {
    std::string src =
        std::string(kRes) + kColorEnum +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var t := 0; "
        "    var n := Color.Red; "
        "    case n of "
        "      Color.Red => do t := t + Sink.Take(move x); end, "
        "      Color.Green => do t := t + Sink.Take(move x); end "
        "    end case; "
        "    x := new Res(9); "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kReassignExpectedMsg));
}

// ===========================================================================
// Cell C (reject-side control, §15): a catch-all arm IS present but GUARDED
// (`_ when ...`) -- §19.6 "an arm carrying a `when` guard contributes
// nothing" -- so the case is NOT exhaustive and must still reject. Pins that
// the widened carve-out does not treat a guarded wildcard as a proof.
// ===========================================================================
BEZEL_TEST(case_stmt_exhaustive_no_wildcard_join_4139,
           enum_guarded_catch_all_not_exhaustive_reassign_still_rejected) {
    std::string src =
        std::string(kRes) + kColorEnum +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var t := 0; "
        "    var n := Color.Red; "
        "    case n of "
        "      Color.Red => do t := t + Sink.Take(move x); end, "
        "      _ when t > 100 => do t := t + Sink.Take(move x); end "
        "    end case; "
        "    x := new Res(9); "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kReassignExpectedMsg));
}

// ===========================================================================
// Cell D (reject-side control, §15): a `closed` hierarchy with a subclass
// (`Cat`) left UNHANDLED -- only `Dog` is matched -- must still reject.
// ===========================================================================
BEZEL_TEST(case_stmt_exhaustive_no_wildcard_join_4139,
           sealed_hierarchy_missing_leaf_reassign_still_rejected) {
    std::string src =
        std::string(kRes) + kAnimalHierarchy +
        "class C "
        "  public static method Main(a: Animal): int "
        "    var x := new Res(5); "
        "    var t := 0; "
        "    case a of "
        "      var d: Dog => do t := t + Sink.Take(move x); end "
        "    end case; "
        "    x := new Res(9); "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kReassignExpectedMsg));
}

// ===========================================================================
// Cell E: choice, exhaustive, NO wildcard -- the other closed-value-space
// kind -- MUST ACCEPT, sibling of Cell A.
// ===========================================================================
BEZEL_TEST(case_stmt_exhaustive_no_wildcard_join_4139,
           choice_exhaustive_no_wildcard_every_arm_consumes_reassign_accepted) {
    std::string src =
        std::string(kRes) +
        "choice Shape "
        "  Circle(r: int), "
        "  Square(s: int); "
        "end Shape; "
        "class C "
        "  public static method Main(sh: Shape): int "
        "    var x := new Res(5); "
        "    var t := 0; "
        "    case sh of "
        "      Circle(r) => do t := t + Sink.Take(move x); end, "
        "      Square(s) => do t := t + Sink.Take(move x); end "
        "    end case; "
        "    x := new Res(9); "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// Cell F (reject-side control, §15): a `case` over an OPEN (non-sealed,
// non-closed) type -- exhaustiveness cannot be proven at all -- must still
// reject, whether or not the one arm present happens to consume `x`.
// ===========================================================================
BEZEL_TEST(case_stmt_exhaustive_no_wildcard_join_4139,
           open_hierarchy_no_wildcard_reassign_still_rejected) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(o: Object): int "
        "    var x := new Res(5); "
        "    var t := 0; "
        "    case o of "
        "      var r: Res => do t := t + Sink.Take(move x); end "
        "    end case; "
        "    x := new Res(9); "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kReassignExpectedMsg));
}

// ===========================================================================
// Lambda-body twin of Cell A (§28.3 K1.3/K7, v0.298): the SAME fix applies
// to `applyUsingVarBorrowPinInLambdaStmt`'s CaseStmt arm (Ownership.cpp),
// which reads the identical `noMatchEdgeStaticallyDead` field.
//
// NOTE on shape: this row asserts MESSAGE-TEXT DISCRIMINATION on a bare READ
// after the `case`, not "reassign accepted". A reassign-then-dispose variant
// (mirroring Cell A's real per-method shape) was tried first and hit an
// UNRELATED, pre-existing gap filed separately as #4187: the lambda-body
// pre-pass's `AssignmentStmt` arm (Ownership.cpp ~9107) never updates
// `states_` for the assignment TARGET (only the RHS), so ANY reassignment of
// a local inside a lambda body -- independent of this fix, independent of
// `case` entirely, reproduced with one straight-line unconditional `move` and
// no join -- leaves the target's PRE-assignment state stale, falsely
// rejecting the next touch. A bare read cleanly isolates THIS fix's own
// effect instead: on the baseline (pre-#4139) join, `x` is `maybe-consumed`
// after the case (the dead no-match edge was wrongly folded in), so the read
// draws "may have already been consumed on another branch ... (maybe-
// consumed join, §28.3)"; with this fix, `x` is DEFINITELY MovedFrom (every
// arm consumed it, no-match edge correctly excluded), so the SAME read draws
// the different, correct-for-a-definite-consume message "was already moved
// out at this point (§28.3)" -- proving the join's classification changed
// exactly as intended, without touching #4187's separate gap.
// ===========================================================================
BEZEL_TEST(case_stmt_exhaustive_no_wildcard_join_4139,
           lambda_body_enum_exhaustive_no_wildcard_read_after_definitely_moved) {
    std::string src =
        std::string(kRes) + kColorEnum +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(n: Color): int => do "
        "      var x := new Res(5); "
        "      var t := 0; "
        "      case n of "
        "        Color.Red => do t := t + Sink.Take(move x); end, "
        "        Color.Green => do t := t + Sink.Take(move x); end, "
        "        Color.Blue => do t := t + Sink.Take(move x); end "
        "      end case; "
        "      t := t + Sink.Peek(x); "
        "      return t; "
        "    end; "
        "    return f(Color.Red); "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "owned value in `x` was already moved out at this point (§28.3)"));
    BEZEL_EXPECT_FALSE(hasErrorMessage(
        r, "may have already been consumed on another branch"));
}

// ===========================================================================
// Real-execution row (runAotChecked), LOOP-AMPLIFIED 500x per
// [[lsan-one-shot-leak-tests-pass-vacuously]]: the enum-exhaustive Cell A
// shape, alternating which arm runs across iterations so all three are
// exercised, asserting exact-once disposal (`Res.live == 0`) -- proves the
// widened carve-out causes no leak, no double-free, and no false reject at
// runtime, not just at the diagnostic layer.
// ===========================================================================
BEZEL_TEST(case_stmt_exhaustive_no_wildcard_join_4139,
           enum_exhaustive_no_wildcard_disposes_once_amplified) {
    std::string src =
        std::string(kRes) + kColorEnum +
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var t := 0; "
        "    while i < 500 do "
        "      var x := new Res(5); "
        "      var m := i mod 3; "
        "      var n := Color.Red; "
        "      if m = 1 then n := Color.Green; end; "
        "      if m = 2 then n := Color.Blue; end; "
        "      case n of "
        "        Color.Red => do t := t + Sink.Take(move x); end, "
        "        Color.Green => do t := t + Sink.Take(move x); end, "
        "        Color.Blue => do t := t + Sink.Take(move x); end "
        "      end case; "
        "      x := new Res(9); "
        "      x.Dispose(); "
        "      i := i + 1; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
