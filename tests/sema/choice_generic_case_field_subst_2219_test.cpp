// Spec-derived sema tests for #2219: a generic CHOICE's positional case-
// destructuring slot must be bound at the per-instantiation generic-arg
// type, NOT at the case's unresolved bare-type-parameter declaration form.
//
// §35.5 "Case patterns": a positional destructuring `CaseName(slot, …)`
// over a CHOICE binds each slot at the case's PAYLOAD FIELD TYPE. When the
// CHOICE is generic (`Both<T, E>`) and the SCRUTINEE carries concrete
// instantiation arguments (`Both<int, string>`), the bare-type-parameter
// payload type (`T`, `E`) MUST be substituted by the scrutinee's args at
// every destructuring slot. The §20 common-type computation between arms
// depends on it: without substitution the arms bind their slot at unrelated
// bare-type-parameter Named refs (`T` and `E`), and `commonTypeOf` rejects
// the unrelated named types with "the arms of a `case` expression have no
// common type…" — even though both arms return `int`, `int`.
//
// The fix's substitution walker is also tested for non-bare shapes:
// `Array<T>` payload at `ArrayChoice<int>` substitutes to `Array<int>`, so
// `xs.Length` resolves on `xs` typed `Array<int>` (not bare `T`).

#include <string>
#include <string_view>

#include "ast/Node.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult semaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool lacksErrDiagWith(const bezel::test::SemaResult& r,
                      std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return false;
    }
    return true;
}

bool hasErrDiagWith(const bezel::test::SemaResult& r,
                    std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

bool reportsUnresolved(const bezel::test::SemaResult& r,
                       std::string_view name) {
    return hasErrDiagWith(r, std::string("unresolved identifier `") +
                                 std::string(name) + "`");
}

}  // namespace

// ----- §35.5 CaseExpr over a generic choice (expression position) -----------

// (1) The headline repro: `case b of Good(v) => v, Bad(e) => e` over
// `Both<int, int>` — both arms return `int`, so the §20 common type is
// `int`. Without the #2219 fix the arms bound `v: T` and `e: E` (different
// bare type-params), and `commonTypeOf` rejected "no common type". FAILS
// WITHOUT the fix; PASSES WITH it.
BEZEL_TEST(choice_generic_case_field_subst_2219,
           expr_both_int_int_arms_share_substituted_int_spec_35_5) {
    auto r = semaAll(
        "module M; "
        "choice Both<T, E> Good(value: T), Bad(err: E); end Both; "
        "class C "
        "  public static method Of(b: Both<int, int>): int => "
        "    (case b of "
        "      Good(v) => v, "
        "      Bad(e)  => e); "
        "end C;");
    // No "no common type" diagnostic, no "unresolved identifier v".
    BEZEL_EXPECT_TRUE(lacksErrDiagWith(r, "no common type"));
    BEZEL_EXPECT_FALSE(reportsUnresolved(r, "v"));
    BEZEL_EXPECT_FALSE(reportsUnresolved(r, "e"));
}

// (1s) Statement-position analogue of (1): a `case` statement over
// `Both<int, int>` whose arms both `return v;` / `return e;`. The
// destructured slot must be `int` so the return-statement checks pass.
// Tests the CaseStmt analogue of the fix.
BEZEL_TEST(choice_generic_case_field_subst_2219,
           stmt_both_int_int_returns_succeed_spec_35_5) {
    auto r = semaAll(
        "module M; "
        "choice Both<T, E> Good(value: T), Bad(err: E); end Both; "
        "class C public static method Of(b: Both<int, int>): int "
        "  case b of "
        "    Good(v) => do return v; end, "
        "    Bad(e)  => do return e; end "
        "  end; "
        "  return 0; "
        "end Of; end C;");
    BEZEL_EXPECT_TRUE(lacksErrDiagWith(r, "no common type"));
    BEZEL_EXPECT_FALSE(reportsUnresolved(r, "v"));
    BEZEL_EXPECT_FALSE(reportsUnresolved(r, "e"));
}

// (2) The stdlib's prelude-injected `Result<T, E>` at `Result<int, string>`.
// Without the fix, `case r of Ok(v) => v, Error(e) => 0` was rejected with
// the same §20 diagnostic (the prelude source lives at
// `src/driver/Driver.cpp:61`). FAILS WITHOUT the fix; PASSES WITH it.
BEZEL_TEST(choice_generic_case_field_subst_2219,
           expr_result_int_string_ok_v_returns_int_spec_35_5) {
    auto r = semaAll(
        "module M; "
        "class C public static method Of(r: Result<int, string>): int => "
        "  (case r of "
        "    Ok(v)    => v, "
        "    Error(_) => 0); "
        "end C;");
    BEZEL_EXPECT_TRUE(lacksErrDiagWith(r, "no common type"));
}

// (3) Type substitution in the inner payload type: `Has(xs: Array<T>)` at
// `ArrayChoice<int>` substitutes the inner `Array<T>` to `Array<int>`, so
// `xs.Length` resolves (the inner `T` is replaced by `int`, NOT by the
// bare-type-parameter symbol's `T`). Exercises the recursive `genericArgs`
// walk in `ExprTC::substituteGenericFieldType` (renamed by fix #3898 from
// `substituteGenericCaseFieldType` when generalized to also serve generic
// records; the walk itself is unchanged).
BEZEL_TEST(choice_generic_case_field_subst_2219,
           expr_array_choice_inner_t_substituted_spec_35_5) {
    auto r = semaAll(
        "module M; "
        "choice ArrayChoice<T> Has(xs: Array<T>); end ArrayChoice; "
        "class C public static method Of(c: ArrayChoice<int>): int => "
        "  (case c of "
        "    Has(xs) => xs.Length); "
        "end C;");
    // Without the substitution, `xs` is bound at bare `T` and `Length` is
    // unresolved on the bare-type-parameter ref.
    BEZEL_EXPECT_FALSE(reportsUnresolved(r, "Length"));
}

// (4) Substitution also applies when the slot uses an explicit `var x: T`
// binding pattern. The binder's own `T` comes from the scrutinee's
// instantiation (the `T` here names the CHOICE's generic, not the
// binder's local) — sema's existing §19.5 typed-binding rule sees the
// scrutinee TYPE as `Both<int,int>`, so the typed binder `var v: int` at
// `Both<int,int>` is a type-test+bind pattern (#1913) — that path is
// independent of our fix; this test pins NON-regression.
BEZEL_TEST(choice_generic_case_field_subst_2219,
           expr_typed_binding_at_generic_scrutinee_spec_35_5) {
    auto r = semaAll(
        "module M; "
        "choice Both<T, E> Good(value: T), Bad(err: E); end Both; "
        "class C public static method Of(b: Both<int, int>): int => "
        "  (case b of "
        "    Good(var v: int) => v, "
        "    Bad(var e: int)  => e); "
        "end C;");
    BEZEL_EXPECT_TRUE(lacksErrDiagWith(r, "no common type"));
}

// (5) NEGATIVE case to guard against over-substitution: a bare `T`-binder
// at `Both<int,int>` is a typed-binding pattern whose declared type is the
// bare type-parameter — sema accepts it (per §19.3 the type-parameter
// itself is a valid type ascription), but the binder is a type-test
// pattern that always succeeds (a runtime equivalence test on the bare
// parameter tags identically with `int` — see #1913 acceptance window).
// This test pins that the substitution does NOT silently rewrite a
// USER-WRITTEN `T` binder into `int` (the user typed `T`, they get `T`
// back, even though the slot's underlying T is `int`). The fix only
// substitutes the IMPLICIT field type from `cc->caseDecl->payload[i].type`.
BEZEL_TEST(choice_generic_case_field_subst_2219,
           negative_binder_typed_T_remains_bare_T_spec_35_5) {
    // `var x: T` at `Both<int,int>` is the #1913-determined shape — the
    // binder IS the bare `T` (sema tests #1913's acceptance). It returns
    // an `int`-compatible (post-substitution) value back through the case
    // only when the matching arm's body reads a `T`-typed slot; since the
    // slot binder here is `x: T` (the same Name as the choice param), the
    // slot binding stays `T` and the arm returns `T`, and the body's
    // `x + 0` may not type-check. Most importantly: no spurious "no common
    // type" between the two arms (both at `T` would unify). This test
    // exists to pin the scope of the fix.
    auto r = semaAll(
        "module M; "
        "choice Both<T, E> Good(value: T), Bad(err: E); end Both; "
        "class C public static method Of(b: Both<int, int>): int => "
        "  (case b of "
        "    Good(var v: T) => 0, "
        "    Bad(var e: T)  => 0); "
        "end C;");
    // Both arms return `int`; no spurious "no common type".
    BEZEL_EXPECT_TRUE(lacksErrDiagWith(r, "no common type"));
}

// (6) Mixed payload kinds: `Ok(value: T), Error(err: E)` at
// `Mixed<int, string>` — arms return `int` and `string.Length` (which is
// `int`), so the common type is `int` after the fix. Pins the substitution
// applied to BOTH cases when the args have DIFFERENT ground types (would
// over-substitute in any but T→int / E→string).
BEZEL_TEST(choice_generic_case_field_subst_2219,
           expr_mixed_int_string_arms_share_int_spec_35_5) {
    auto r = semaAll(
        "module M; "
        "choice Mixed<T, E> Ok(value: T), Err(msg: E); end Mixed; "
        "class C public static method Of(m: Mixed<int, string>): int => "
        "  (case m of "
        "    Ok(v)  => v, "
        "    Err(s) => s.Length); "
        "end C;");
    BEZEL_EXPECT_TRUE(lacksErrDiagWith(r, "no common type"));
    BEZEL_EXPECT_FALSE(reportsUnresolved(r, "Length"));
}
