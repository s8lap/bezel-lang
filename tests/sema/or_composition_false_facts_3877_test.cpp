// Spec-derived tests for issue #3877 — §7.4's `or` half of "boolean
// composition of tests" was unimplemented: in `A or B`, `B` (and the false
// branch) must see `A`'s false-facts, mirroring the already-implemented `and`
// half (`A and B`'s `B`, and true branch, see `A`'s true-facts, #2212).
//
// Spec §7.4 (v0.294), "How narrowing is established":
//   "boolean composition of tests: in `A and B`, `B` (and the true branch) is
//   checked under `A`'s true-facts; in `A or B`, `B` (and the false branch)
//   sees `A`'s false-facts; `not` swaps the fact sets"
// and the same section's null-test bullet lists `x = null` as narrowing "the
// *false* branch" — so in `s = null or s.Length > 0`, the right operand (and
// the containing `if`'s `else`) should see `s`'s false-fact from `s = null`,
// i.e. `s <> null`.
//
// Measured on HEAD before this fix (commit 922e754a): `s = null or
// s.Length > 0` was rejected --- "member access `.Length` on a nullable
// value requires narrowing it to non-null first (§7.4)" --- while the dual
// `s <> null and s.Length > 0` compiled clean. `.Length` is used throughout
// (never `String.IsEmpty`) to keep this isolated from #2348's separate
// codegen gap, per the filed issue's own guidance.
//
// The fix: ExprTypecheck_Exprs.cpp's `visitChildren` BinaryExpr handling
// gained an `or` arm (mirroring the existing `and` arm) that seeds the right
// operand's visit with the left operand's FALSE-facts via
// `collectNarrowings(left, forTrueBranch=false)`, and ExprTypecheck.cpp's
// `collectNarrowings` gained an `or && !forTrueBranch` arm so an `if`'s
// `else` branch (or a conditional atom's `else` value) also receives both
// operands' combined false-facts — the De Morgan dual of the existing
// `and && forTrueBranch` arm.
//
// Rows below, matching the issue's requested coverage:
//   (a) the exact repro, as an if-EXPRESSION condition (right operand sees
//       the left's false-fact);
//   (b) the same, as an if-STATEMENT condition (right operand);
//   (c) the if-STATEMENT's `else` BRANCH BODY, not just the right operand —
//       both operands' false-facts apply there per §7.4;
//   (d) a three-way nested `or` chain — the rightmost operand sees the
//       combined false-facts of every preceding disjunct;
//   (e) NEGATIVE: a different variable in the right operand must still
//       reject (no fact was ever established about it);
//   (f) NEGATIVE: wrong polarity (`<>` instead of `=`, still joined by `or`)
//       must still reject — `s <> null`'s FALSE-branch establishes nothing
//       (the system tracks only known-non-null facts, and when `s <> null`
//       is false, `s` actually IS null, so accepting here would be unsound);
//   (g) NEGATIVE: the `or`'s TRUE branch (a conditional atom's `then` value,
//       or the controlled `if` body) gets no narrowing — neither disjunct is
//       guaranteed, symmetric to the already-deferred false branch of `and`;
//   (h) the #2258 invalidation-preserving analogue for `or`: a `ref`
//       argument in the right operand must still drop the fact for
//       everything after the `or` expression — the restore is an
//       INTERSECTION with the entry snapshot, never an assignment of it;
//   (i) control: the dual `and`/`<>` form still compiles (no regression).

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some reported diagnostic's message contains `needle`.
bool hasMsg(const bezel::test::SemaResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// True iff some diagnostic carries `needle` at a REAL (non-zero) source
// location — this test drives the sema pipeline directly (parseAndCollect +
// typecheckExpressions), never the AOT harness, so DiagnosticEngine's
// locations are the real `SourceLocation`s the checker reported, not the
// AOT harness's synthetic {0,0} packing.
bool hasMsgAtRealLocation(const bezel::test::SemaResult& r,
                          const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find(needle) == std::string::npos) continue;
        if (d.location.line != 0 || d.location.column != 0) return true;
    }
    return false;
}

const char* kNarrowMsg = "requires narrowing";

// A `ref`-taking helper that nulls its argument — the §7.4 invalidation form
// "the variable is passed as a `ref` or `out` argument", spelled once
// (mirrors narrowing_restore_intersection_2258_test.cpp's kClearDecls).
const char* kClearDecl =
    "  public static method Clear(ref s: string?): bool "
    "    s := null; return true; "
    "  end Clear; ";

}  // namespace

// ---------------------------------------------------------------------------
// (a) if-EXPRESSION condition: the repro exactly as filed.
// ---------------------------------------------------------------------------
BEZEL_TEST(or_composition_false_facts_3877,
           accept_if_expr_condition_rhs_sees_lhs_false_fact_spec_7_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method F(s: string?): int "
        "    return (if (s = null or s.Length > 0) then 1 else 0); "
        "  end F; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// (b) if-STATEMENT condition: the same right-operand narrowing, used inside
//     a plain `if` (not a conditional atom).
// ---------------------------------------------------------------------------
BEZEL_TEST(or_composition_false_facts_3877,
           accept_if_stmt_condition_rhs_sees_lhs_false_fact_spec_7_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method F(s: string?): int "
        "    if s = null or s.Length > 0 then return 1; end; "
        "    return 0; "
        "  end F; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// (c) The FALSE BRANCH (the if-statement's `else` body) — §7.4 grants the
//     combined false-facts to the false branch too, not just the right
//     operand of the condition itself. By De Morgan, `s = null or
//     s.Length > 0` being false means BOTH disjuncts are false, so `s`'s
//     false-fact from `s = null` (i.e. `s <> null`) holds in `else`.
// ---------------------------------------------------------------------------
BEZEL_TEST(or_composition_false_facts_3877,
           accept_if_stmt_else_branch_sees_combined_false_facts_spec_7_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method F(s: string?): int "
        "    if (s = null or s.Length > 0) then "
        "      return 1; "
        "    else "
        "      return s.Length; "
        "    end; "
        "  end F; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// (d) Nested `or` chain: the rightmost disjunct sees the combined
//     false-facts of every PRECEDING disjunct (collectNarrowings recurses
//     into a nested `or` the same way the `and` side already recurses).
// ---------------------------------------------------------------------------
BEZEL_TEST(or_composition_false_facts_3877,
           accept_nested_or_chain_rhs_sees_all_preceding_false_facts_spec_7_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method F(a: string?, b: string?): int "
        "    if a = null or b = null or (a.Length + b.Length) > 0 then "
        "      return 1; "
        "    end; "
        "    return 0; "
        "  end F; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// (e) NEGATIVE — a DIFFERENT variable in the right operand: no fact was
//     established about `t`, so `t.Length` must still reject.
// ---------------------------------------------------------------------------
BEZEL_TEST(or_composition_false_facts_3877,
           reject_rhs_different_variable_no_fact_established_spec_7_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method F(s: string?, t: string?): int "
        "    return (if (s = null or t.Length > 0) then 1 else 0); "
        "  end F; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsgAtRealLocation(r, kNarrowMsg));
}

// ---------------------------------------------------------------------------
// (f) NEGATIVE — wrong polarity: `s <> null or s.Length > 0`. The left
//     disjunct's FALSE branch (`s <> null` being false, i.e. `s` IS null)
//     establishes nothing — accepting here would be UNSOUND (the right
//     operand runs exactly when `s` is null). Must still reject.
// ---------------------------------------------------------------------------
BEZEL_TEST(or_composition_false_facts_3877,
           reject_wrong_polarity_diamond_null_test_still_rejects_spec_7_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method F(s: string?): int "
        "    return (if (s <> null or s.Length > 0) then 1 else 0); "
        "  end F; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsgAtRealLocation(r, kNarrowMsg));
}

// ---------------------------------------------------------------------------
// (g) NEGATIVE — the `or`'s TRUE branch gets no narrowing (neither disjunct
//     is guaranteed to be the one that held), symmetric to the pre-existing
//     deferral of an `and`'s FALSE branch. `then` here executes when the
//     condition is true — either disjunct could be why — so `s` is not
//     provably non-null.
// ---------------------------------------------------------------------------
BEZEL_TEST(or_composition_false_facts_3877,
           reject_or_true_branch_gets_no_narrowing_deferred_spec_7_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method F(s: string?): int "
        "    if s = null or s.Length = 0 then return s.Length; end; "
        "    return 1; "
        "  end F; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsgAtRealLocation(r, kNarrowMsg));
}

// ---------------------------------------------------------------------------
// (h) #2258 invalidation-preserving analogue for `or`: the right operand's
//     `ref` argument invalidates `s`'s narrowing; the restore after the `or`
//     must be an INTERSECTION with the entry snapshot (which lacks the
//     dropped fact), never an assignment of the pre-drop snapshot. Pre-fix
//     code had NO `or` handling at all in `visitChildren`, so this row could
//     not even be posed; it guards the new code from the same bug #2258
//     fixed on the `and` side.
// ---------------------------------------------------------------------------
BEZEL_TEST(or_composition_false_facts_3877,
           reject_or_rhs_ref_drop_survives_the_operator_spec_7_4) {
    auto r = parseSemaAll(
        std::string("module M; class C ") + kClearDecl +
        "  public static method Run(t: string?): int "
        "    var s: string? := t; "
        "    if s = null then return 0; end; "
        "    var b: bool := (s = null or C.Clear(ref s)); "
        "    return s.Length; "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsgAtRealLocation(r, kNarrowMsg));
}

// ---------------------------------------------------------------------------
// (i) Control — the dual `and`/`<>` form (already implemented since #2212)
//     still compiles clean; this fix must not disturb it.
// ---------------------------------------------------------------------------
BEZEL_TEST(or_composition_false_facts_3877,
           accept_and_dual_form_unaffected_spec_7_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method F(s: string?): int "
        "    return (if (s <> null and s.Length > 0) then 1 else 0); "
        "  end F; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMsg(r, kNarrowMsg));
}
