// Spec-derived tests for issue #2105 — v0.210 §7.4 narrowing establishment
// inside the §20 conditional atom's branches and inside a scrutinee-less
// `when` arm's own body.
//
// §7.4 (v0.210 addition, "How narrowing is established"), first bullet:
//
//   > a null test — `x <> null` (narrowing the true branch), `x = null`
//   > (narrowing the *false* branch), `x is not null`, or a successful
//   > `is`/typed-pattern test (§12.7, §19.3) — in an `if`/`elif` condition
//   > (of the statement or of the §20 conditional atom, each condition
//   > narrowing its own branches), a `while` condition, a `when` guard,
//   > or a `(when …)` arm condition (§19.1, narrowing that arm's body);
//
// Pre-#2105 the v0.210 narrowing-establishment rule was missing for the two
// expression-position surfaces that codegen (and the LIT conformance
// corpus) had reached the user through:
//   (A) the §20 conditional atom: `(if c then x else y)` — the condition
//       did not narrow `then`, so `s <> null => s.Length else 0` rejected
//       with "member access `.Length` on a nullable value requires
//       narrowing";
//   (B) the scrutinee-less `when` arm: `(when s <> null => s.Length,
//       _ => 0)` — the arm's own condition did not narrow the arm body,
//       same reject.
//
// Both surfaces are PURE SEMANTIC — the compiler must ACCEPT the narrowed
// member access. The complementary REJECT direction (the false branch of
// (A), and a non-first arm of (B) — neither of which is reached when its
// preceding condition was true) must STILL REJECT: the narrowing does not
// leak across branch boundaries.
//
// The fix is in ExprTypecheck_Exprs.cpp's `ConditionalExpr` visit handler
// (A) and its scrutinee-less `CaseExpr` arm body handler (B-expression),
// and in ExprTypecheck.cpp's scrutinee-less `CaseStmt` arm body handler
// (B-statement). Each mirrors the IfStmt save/restore pattern from
// ExprTypecheck.cpp, calling `collectNarrowings(cond, forTrueBranch)`
// around the body visit.

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

}  // namespace

// ---------------------------------------------------------------------------
// §7.4 v0.210 / §20 conditional atom — the condition narrows the THEN branch.
// ---------------------------------------------------------------------------

// §7.4 (v0.210, #2105): the §20 conditional atom's condition narrows its
// OWN branches exactly as an if-STATEMENT condition narrows its body.
// `(if s <> null then s.Length else 0)` is the spec's canonical example
// (post-#2079 paren-atom surface); the THEN branch sees `s` as non-null
// and `s.Length` is accepted without a further guard.
BEZEL_TEST(conditional_atom_narrow_2105, if_atom_then_branch_accepts_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Run(s: string?): int "
        "    return (if s <> null then s.Length else 0); "
        "  end Run; "
        "  public static method Main(): int "
        "    return C.Run(\"hi\"); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.4 / §20 (control, #2105): the ELSE branch is NOT narrowed by the
// condition's TRUE branch — `s <> null` narrows `s` to non-null only on
// the THEN side. `s.Length` in the ELSE branch is reachable when `s = null`,
// so the original §7.4 reject must still fire.
BEZEL_TEST(conditional_atom_narrow_2105, if_atom_else_branch_unnarrowed_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Run(s: string?): int "
        "    return (if s <> null then 0 else s.Length); "
        "  end Run; "
        "  public static method Main(): int "
        "    return C.Run(\"hi\"); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "on a nullable value requires narrowing"));
}

// §7.4 / §20 (#2105): `s = null` narrows the FALSE branch (the spec
// symmetric direction) — `(if s = null then 0 else s.Length)` accepts
// `s.Length` in the ELSE branch because `s` is non-null there.
BEZEL_TEST(conditional_atom_narrow_2105, if_atom_eq_null_narrows_else_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Run(s: string?): int "
        "    return (if s = null then 0 else s.Length); "
        "  end Run; "
        "  public static method Main(): int "
        "    return C.Run(\"hi\"); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §7.4 v0.210 / §19.1 — scrutinee-less `when` arm's own condition
// narrows THAT arm's own body.
// ---------------------------------------------------------------------------

// §7.4 (v0.210, #2105): the first arm's condition narrows the first
// arm's body. `(when s <> null => s.Length, _ => 0)` — `s.Length` in the
// first arm is accepted without a further guard.
// v0.289 (§19, §22.4, Appendix A.6/A.7, proposal 0122 L1): the `(when …)`
// expression is retired — rewritten as an equivalent `(if … then … else …)`
// to preserve the narrowing subject.
BEZEL_TEST(when_arm_narrow_2105, when_expr_first_arm_condition_narrows_body_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Run(s: string?): int "
        "    return (if s <> null then s.Length else 0); "
        "  end Run; "
        "  public static method Main(): int "
        "    return C.Run(\"hi\"); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.4 / §19.1 (control, #2105): the SECOND arm does not inherit the
// FIRST arm's narrowing — it is reached only when the first arm's
// condition was FALSE (`s = null`). `s.Length` in the `_` arm is the
// original §7.4 reject.
BEZEL_TEST(when_arm_narrow_2105, when_expr_second_arm_unnarrowed_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Run(s: string?): int "
        "    return (when s <> null => 0, _ => s.Length); "
        "  end Run; "
        "  public static method Main(): int "
        "    return C.Run(\"hi\"); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "on a nullable value requires narrowing"));
}

// §7.4 / §19.1 / statement-position (#2105): the statement-position
// `when` arm's own condition narrows that arm's own body — the parser
// produces a CaseStmt (not CaseExpr) for this surface, so this case
// pins the CaseStmt scrutinee-less handler in ExprTypecheck.cpp
// (the expression counterpart is pinned above).
// v0.289 (§19, §22.4, Appendix A.6/A.7, proposal 0122 L1): the `when … end;`
// statement is retired — converted to rejection (no equivalent statement form
// preserves the arm-narrowing semantics; the narrowing subject is covered by
// the `(if …)` expression form in `conditional_atom_narrow_2105`).
BEZEL_TEST(when_arm_narrow_2105, when_stmt_first_arm_condition_narrows_body_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Run(s: string?): int "
        "    var r: int := 0; "
        "    when s <> null => r := s.Length, "
        "         _         => r := 0, "
        "    end; "
        "    return r; "
        "  end Run; "
        "  public static method Main(): int "
        "    return C.Run(\"hi\"); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());  // v0.289: `when … end;` retired
}

// §7.4 / §19.1 / statement-position (control, #2105): the discard
// arm does not see the previous arm's narrowing.
BEZEL_TEST(when_arm_narrow_2105, when_stmt_second_arm_unnarrowed_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Run(s: string?): int "
        "    var r: int := 0; "
        "    when s <> null => r := 0, "
        "         _         => r := s.Length, "
        "    end; "
        "    return r; "
        "  end Run; "
        "  public static method Main(): int "
        "    return C.Run(\"hi\"); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "on a nullable value requires narrowing"));
}
