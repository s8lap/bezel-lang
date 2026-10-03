// Spec-derived regression tests for fix #5235 — §19.3 (v0.313).
//
// A relational or range pattern over an Object scrutinee lowers to malformed IR
// (`icmp sgt ptr %obj, i32 5`) because Object has no defined ordering. The
// spec says relational patterns compare under the scrutinee's type ordering
// (§19.3), and Object has no ordering. The compiler must reject at sema rather
// than emitting malformed IR.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeNotRuntime;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

bool hasObjectOrderingDiag(const bezel::test::JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("Object has no defined ordering") !=
                std::string::npos)
            return true;
    return false;
}

}  // namespace

// §19.3 (v0.313, #5235): RelOpPattern over Object — REJECT
BEZEL_TEST(object_scrutinee_relop_pattern_rejected_5235,
           relop_gt_over_object_rejected) {
    auto r = runAot(
        "module M; class P "
        "  public static method Main(): int "
        "    var o: Object := null; "
        "    return (case o of > 5 => 1, _ => 2); "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasObjectOrderingDiag(r));
}

// §19.3 (v0.313, #5235): RelOpPattern over Object (>=) — REJECT
BEZEL_TEST(object_scrutinee_relop_pattern_rejected_5235,
           relop_gte_over_object_rejected) {
    auto r = runAot(
        "module M; class P "
        "  public static method Main(): int "
        "    var o: Object := null; "
        "    return (case o of >= 5 => 1, _ => 2); "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasObjectOrderingDiag(r));
}

// §19.3 (v0.313, #5235): RelOpPattern over Object (<) — REJECT
BEZEL_TEST(object_scrutinee_relop_pattern_rejected_5235,
           relop_lt_over_object_rejected) {
    auto r = runAot(
        "module M; class P "
        "  public static method Main(): int "
        "    var o: Object := null; "
        "    return (case o of < 5 => 1, _ => 2); "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasObjectOrderingDiag(r));
}

// §19.3 (v0.313, #5235): RelOpPattern over Object (<=) — REJECT
BEZEL_TEST(object_scrutinee_relop_pattern_rejected_5235,
           relop_lte_over_object_rejected) {
    auto r = runAot(
        "module M; class P "
        "  public static method Main(): int "
        "    var o: Object := null; "
        "    return (case o of <= 5 => 1, _ => 2); "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasObjectOrderingDiag(r));
}

// §19.3 (v0.313, #5235): RangePattern over Object — REJECT
BEZEL_TEST(object_scrutinee_relop_pattern_rejected_5235,
           range_over_object_rejected) {
    auto r = runAot(
        "module M; class P "
        "  public static method Main(): int "
        "    var o: Object := null; "
        "    return (case o of 1..10 => 1, _ => 2); "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasObjectOrderingDiag(r));
}

// Control: a discard pattern over Object compiles cleanly — the fix fires
// only on RelOpPattern and RangePattern, not on other pattern kinds.
BEZEL_TEST(object_scrutinee_relop_pattern_rejected_5235,
           discard_pattern_over_object_compiles) {
    auto r = runAot(
        "module M; class P "
        "  public static method Main(): int "
        "    var o: Object := null; "
        "    return (case o of _ => 1); "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §19.3 / §16.6 (v0.313, #5235): the STATEMENT-position form (`case … of …
// end case;`, A.6 CaseStmt) is the OTHER spelling of the same construct. It
// has its own visit site (ExprTypecheck.cpp) and, with the expression-position
// gate alone, still reached codegen and emitted `icmp sgt ptr %obj, i32 5`
// (MEASURED against bezelc -c on this branch's parent). Same rule, same
// diagnostic — the two spellings must not drift.
BEZEL_TEST(object_scrutinee_relop_pattern_rejected_5235,
           relop_over_object_stmt_rejected) {
    auto r = runAot(
        "module M; class P public static method Main(): int "
        "  var o: Object := null; "
        "  case o of "
        "    > 5 => return 1;, "
        "    _ => return 2; "
        "  end case; "
        "  return 0; "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasObjectOrderingDiag(r));
}

// §19.3 / §16.6 (v0.313, #5235): range pattern, statement-position form. The
// stmt-position range arm emits TWO malformed compares pre-fix (`icmp sge ptr
// …, i32 1` and `icmp sle ptr …, i32 10`).
BEZEL_TEST(object_scrutinee_relop_pattern_rejected_5235,
           range_over_object_stmt_rejected) {
    auto r = runAot(
        "module M; class P public static method Main(): int "
        "  var o: Object := null; "
        "  case o of "
        "    1..10 => return 1;, "
        "    _ => return 2; "
        "  end case; "
        "  return 0; "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasObjectOrderingDiag(r));
}

// §19.3 / §16.6 (v0.313, #5235, fix-forward): the `|`-ALTERNATION spelling.
// A.7's `AltPattern ::= Pattern ("|" Pattern)*` explicitly admits relationals
// and ranges as `|`-joined members, so `> 5 | < 1` is an ordinary spelling of
// the very pattern this file gates — not a corner. The gate originally
// dynamic_cast ONE node per arm, which returned null on the AltPattern that
// wraps the two RelOpPatterns, so the arm reached codegen and emitted
// `icmp sgt ptr %1, i32 5` / `icmp slt ptr %1, i32 1` before verifyModule
// failed with "Both operands to ICmp instruction are not of the same type!".
// Same rule, same diagnostic — the alternation is not a second policy.
BEZEL_TEST(object_scrutinee_relop_pattern_rejected_5235,
           alt_relop_over_object_rejected) {
    auto r = runAot(
        "module M; class P "
        "  public static method Main(): int "
        "    var o: Object := null; "
        "    return (case o of > 5 | < 1 => 1, _ => 2); "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasObjectOrderingDiag(r));
}

// The same alternation in the STATEMENT-position spelling. Two `case` visit
// sites take this one shared gate, so both spellings are pinned here for the
// alternation exactly as they are for the bare relational above.
BEZEL_TEST(object_scrutinee_relop_pattern_rejected_5235,
           alt_relop_over_object_stmt_rejected) {
    auto r = runAot(
        "module M; class P public static method Main(): int "
        "  var o: Object := null; "
        "  case o of "
        "    > 5 | < 1 => return 1;, "
        "    _ => return 2; "
        "  end case; "
        "  return 0; "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasObjectOrderingDiag(r));
}

// CONTROLS — the three ordered scrutinee types the issue names must keep
// compiling. §19.3's rule is about the scrutinee's type HAVING an ordering,
// not about relational patterns as such: `int` is ordered natively, `decimal`
// is ordered, and `string` is ordered by the §4.6 ordinal intrinsics (the
// #5173 route). If any of these rows ever flip to rejected, this fix has
// over-reached.
BEZEL_TEST(object_scrutinee_relop_pattern_rejected_5235,
           control_relop_over_int_compiles) {
    auto r = runAot(
        "module M; class P "
        "  public static method Main(): int "
        "    var x: int := 7; "
        "    return (case x of > 5 => 1, _ => 2); "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(1, r.exitCode);
}

// `decimal` is the third ordered type the issue names. RELATIONAL, not
// range: §16.3 admits a range pattern only over the integer types and `char`
// ("Range<T> restricts T to the integer types and char"), so a decimal RANGE
// is rejected for an unrelated, correct reason and would prove nothing about
// this fix. The relational form is the one §19.3 orders decimal for.
BEZEL_TEST(object_scrutinee_relop_pattern_rejected_5235,
           control_relop_over_decimal_compiles) {
    auto r = runAot(
        "module M; class P "
        "  public static method Main(): int "
        "    var d: decimal := 7.5m; "
        "    return (case d of > 5m => 1, _ => 2); "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(1, r.exitCode);
}

BEZEL_TEST(object_scrutinee_relop_pattern_rejected_5235,
           control_relop_over_string_compiles) {
    auto r = runAot(
        "module M; class P "
        "  public static method Main(): int "
        "    var s: string := \"b\"; "
        "    return (case s of > \"a\" => 1, _ => 2); "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(1, r.exitCode);
}

// §19.3 (v0.313, #5235 fix-forward): the DESTRUCTURING-SLOT spelling. §19.3
// states the same rule for a slot's right-hand side that it states for an arm
// head — "Each slot's right-hand side is a full pattern … a range, a
// relational, …" — and a slot's scrutinee is the SLOT's own declared type.
// `record Q(A: Object)` + `case o of Q(> 5)` is therefore the same §19.3
// violation as `case o of > 5`, reached by a different spelling.
//
// MEASURED on this branch's parent: it compiled through sema and reached
// codegen, which emitted `icmp sgt ptr %16, i32 5` and failed verifyModule
// with "Both operands to ICmp instruction are not of the same type!" — the
// verbatim defect #5235 exists to eliminate. The arm-level walk descends
// `AltPattern::members` and never visits a `DestructuringPattern`, so the slot
// was unreachable to it; the nullable sibling (`Object?`) already covered this
// site (#3692), which is the asymmetry this row pins shut.
BEZEL_TEST(object_scrutinee_relop_pattern_rejected_5235,
           destructuring_slot_relop_over_object_rejected) {
    auto r = runAot(
        "module M; "
        "record Q(A: Object) end Q; "
        "class P "
        "  public static method Main(): int "
        "    var o: Object := null; "
        "    return (case o of Q(> 5) => 1, _ => 2); "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasObjectOrderingDiag(r));
}

// The same slot with the `|`-ALTERNATION spelling. A.7 admits relationals and
// ranges as `|`-joined alternative members in a PositionalPattern too, so
// `Q(> 5 | < 1)` is the two-relational arm spelling inside the slot; pre-fix
// it emitted the malformed compare TWICE. Pinned separately because the slot
// walk is a different code path from the arm walk and an alternation inside it
// must be descended the same way.
BEZEL_TEST(object_scrutinee_relop_pattern_rejected_5235,
           destructuring_slot_alt_relop_over_object_rejected) {
    auto r = runAot(
        "module M; "
        "record Q(A: Object) end Q; "
        "class P "
        "  public static method Main(): int "
        "    var o: Object := null; "
        "    return (case o of Q(> 5 | < 1) => 1, _ => 2); "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasObjectOrderingDiag(r));
}

// CONTROL — the slot gate keys on the SLOT's DECLARED type, exactly as the
// nullable sibling's slot gate does, so widening the check from the arm to the
// slot must not fire on an ordered slot type. `record Q(A: int)` + `Q(> 5)`
// takes the identical AST shape as the reject row above and differs only in
// the slot's declared type; it must still compile and still return 1 (Q(7),
// and 7 > 5). If this row ever flips, the slot check has over-reached.
BEZEL_TEST(object_scrutinee_relop_pattern_rejected_5235,
           destructuring_slot_int_relop_over_ordered_rejected_accepts) {
    auto r = runAot(
        "module M; "
        "record Q(A: int) end Q; "
        "class P "
        "  public static method Main(): int "
        "    var q := new Q(7); "
        "    return (case q of Q(> 5) => 1, _ => 2); "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(1, r.exitCode);
}

// NARROW-SCOPE GUARD. Whether a user type that DECLARES comparison operators
// counts as having "the ordering of the scrutinee's type" (§19.3) is the open
// spec-question in #5269 (owner-ruled, NOT this lane's to decide). This fix
// gates PrimitiveType::Object only, so it must not change what happens to
// such a scrutinee. This row pins that boundary from BOTH sides: the program
// is still rejected (MEASURED on this branch — codegen emits
// `icmp slt %record.V %9, %10` and `verifyModule` fails), and it is NOT
// rejected by the new §19.3 gate. The first assertion is what makes the row
// non-vacuous; the second is the actual scope claim.
//
// `failedAtCompileTimeNotRuntime`, not `...WithRealLocation`: this program's
// rejection is an IR-verification failure raised by the backend, which carries
// no source location, so the location-based helper cannot see it.
BEZEL_TEST(object_scrutinee_relop_pattern_rejected_5235,
           user_declared_ordering_not_gated_5269) {
    auto r = runAot(
        "module M; "
        "record V(X: int) "
        "  public static method operator <(a: V, b: V): bool => a.X < b.X; "
        "  public static method operator >(a: V, b: V): bool => a.X > b.X; "
        "  public static method operator <=(a: V, b: V): bool => a.X <= b.X; "
        "  public static method operator >=(a: V, b: V): bool => a.X >= b.X; "
        "end V; "
        "class P "
        "  public static method Main(): int "
        "    var a := new V(1); "
        "    var b := new V(9); "
        "    return (case a of < b => 1, _ => 2); "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeNotRuntime(r));
    BEZEL_EXPECT_FALSE(hasObjectOrderingDiag(r));
}
