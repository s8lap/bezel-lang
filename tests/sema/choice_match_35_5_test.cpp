// Spec-derived sema tests for §35.5 (v0.200, #2045): choice matching — the
// borrowing match (the default), case patterns, and the payload-bare /
// arity-mismatch rejections. These exercise the CaseStmt + CaseExpr sema
// handlers' new choice branches:
//
//   * §35.5 "Case patterns": a payload-free case is matched by bare name
//     (the §10.6/§19.8 prefix-omission rule extended to choices); a
//     payload case is matched by a positional destructuring pattern with
//     the case's exact payload arity; a payload case may NOT be matched
//     bare.
//   * §35.5 "The borrowing match": payload bindings are copies (value-like
//     choices) or borrows (borrow-bearing/owning) — the default, non-
//     consuming match.
//
// Stage-2 slice 2 covers the SEMA surface (pattern admissibility, binding
// typing, arity check). The OWNING match (`case owned outcome of`), the
// two-phase guard transfer, and the codegen lowering all land in follow-
// up slices — tests here are SEMA-level (parse + collect + resolve +
// typecheck, assert on diagnostics / resolved types), not runtime.

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

using bezel::sema::TypeSymbol;
using bezel::sema::Symbol;
using bezel::sema::TypeRef;
using bezel::test::parseAndCollect;

namespace {

// Parse + collect + resolve + typecheck — mirrors
// choice_sema_classification_35_test.cpp's semaAll.
bezel::test::SemaResult semaAll(std::string_view src) {
    auto r = parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True when `r` carries an error diagnostic whose message contains `needle`.
bool hasErrDiagWith(const bezel::test::SemaResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

// True when `r` has NO error diagnostic whose message contains `needle`.
bool lacksErrDiagWith(const bezel::test::SemaResult& r,
                      std::string_view needle) {
    return !hasErrDiagWith(r, needle);
}

// True when `r` reports an "unresolved identifier `<name>`" error — the
// sema signal that a name did not resolve. Used to detect a binding that
// FAILED to enter its arm scope: if `var v` destructuring did not register
// `v`, then `return v` produces exactly this diagnostic.
bool reportsUnresolved(const bezel::test::SemaResult& r,
                       std::string_view name) {
    return hasErrDiagWith(r, std::string("unresolved identifier `") +
                                 std::string(name) + "`");
}

}  // namespace

// --- §35.5 payload-free case pattern (bare name) ---------------------------

BEZEL_TEST(choice_match_35_5,
           spec_35_5_payload_free_case_pattern_in_stmt_position) {
    // §35.5 "Case patterns": a payload-free case is matched by its bare
    // name. In statement position, a `case x of None => do … end, … end`
    // over a choice scrutinee must accept `None` as a valid pattern.
    auto r = semaAll(
        "module M; choice Opt Some(value: int), None; end Opt; "
        "class C public static method Main(): int "
        "  var x: Opt := Opt.None; "
        "  case x of "
        "    Some(var v) => do return v; end, "
        "    None        => do return 0; end "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    // The bare `None` resolved against the choice's payload-free cases —
    // NOT reported as a type-test failure or an unresolved identifier.
    BEZEL_EXPECT_FALSE(reportsUnresolved(r, "None"));
    BEZEL_EXPECT_TRUE(lacksErrDiagWith(r, "does not name a type"));
}

BEZEL_TEST(choice_match_35_5,
           spec_35_5_payload_free_case_pattern_in_expr_position) {
    // §35.5 (expression-position `case`). Same payload-free bare-name
    // acceptance, but in an expression context.
    auto r = semaAll(
        "module M; choice Opt Some(value: int), None; end Opt; "
        "class C public static method Main(): int "
        "  var x: Opt := Opt.None; "
        "  return (case x of "
        "    Some(var v) => v, "
        "    None        => 0); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(reportsUnresolved(r, "None"));
}

// --- §35.5 payload case destructuring (binds the payload field) ------------

BEZEL_TEST(choice_match_35_5,
           spec_35_5_payload_case_destructure_binds_payload_as_local) {
    // §35.5: a payload case is matched by `CaseName(slot, …)`. The slot
    // binding (`var v`) becomes a local of the arm body typed as the
    // case's payload field. For a value-like choice, the payload binding
    // is a copy. Here `v` must be `int` (the payload field's type) for
    // `return v` to typecheck; a FAILED binding would surface as an
    // "unresolved identifier `v`" error.
    auto r = semaAll(
        "module M; choice Opt Some(value: int), None; end Opt; "
        "class C public static method Main(): int "
        "  var x: Opt := Opt.None; "
        "  case x of "
        "    Some(var v) => do return v; end, "
        "    None        => do return 0; end "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    // The binding `v` resolves — no "unresolved identifier `v`" diagnostic.
    BEZEL_EXPECT_FALSE(reportsUnresolved(r, "v"));
}

BEZEL_TEST(choice_match_35_5,
           spec_35_5_payload_case_destructure_multi_field) {
    // §35.5 positional destructuring with the case's exact arity: a
    // 2-payload case binds two slots. Both names resolve.
    auto r = semaAll(
        "module M; choice Pair First(a: int, b: int), Zero; end Pair; "
        "class C public static method Main(): int "
        "  var x: Pair := Pair.Zero; "
        "  case x of "
        "    First(var a, var b) => do return a + b; end, "
        "    Zero                 => do return 0; end "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(reportsUnresolved(r, "a"));
    BEZEL_EXPECT_FALSE(reportsUnresolved(r, "b"));
}

BEZEL_TEST(choice_match_35_5,
           spec_35_5_payload_case_destructure_naked_binding) {
    // §19.3 slot rules carry over (§35.5 "Case patterns"): a naked
    // identifier slot binds just like `var x`. `First(a, b)` — bare names.
    auto r = semaAll(
        "module M; choice Pair First(a: int, b: int), Zero; end Pair; "
        "class C public static method Main(): int "
        "  var x: Pair := Pair.Zero; "
        "  case x of "
        "    First(a, b) => do return a + b; end, "
        "    Zero         => do return 0; end "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(reportsUnresolved(r, "a"));
    BEZEL_EXPECT_FALSE(reportsUnresolved(r, "b"));
}

BEZEL_TEST(choice_match_35_5,
           spec_35_5_payload_case_destructure_discard_slot) {
    // §35.5 / §19.3: per-slot `_` discards. `Some(_)` discards the
    // payload — the explicit form for "matched, payload ignored." The
    // discard arm itself produces NO pattern-related diagnostic.
    auto r = semaAll(
        "module M; choice Opt Some(value: int), None; end Opt; "
        "class C public static method Main(): int "
        "  var x: Opt := Opt.None; "
        "  case x of "
        "    Some(_) => do return 1; end, "
        "    None    => do return 0; end "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(lacksErrDiagWith(r, "Some(_)"));
    BEZEL_EXPECT_TRUE(lacksErrDiagWith(r, "Payload case"));
    BEZEL_EXPECT_TRUE(lacksErrDiagWith(r, "case-destructuring"));
}

// --- §35.5 payload-bare rejection ------------------------------------------

BEZEL_TEST(choice_match_35_5, spec_35_5_payload_case_bare_is_rejected) {
    // §35.5: "A payload case may not be matched bare: `Fetched` without
    // parentheses is a compile error naming the arity — matching a
    // payload while ignoring it is spelled visibly as `Fetched(_)`."
    auto r = semaAll(
        "module M; choice Opt Some(value: int), None; end Opt; "
        "class C public static method Main(): int "
        "  var x: Opt := Opt.None; "
        "  case x of "
        "    Some      => do return 1; end, "
        "    None      => do return 0; end "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "Payload case `Some`"));
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "1 slot"));
}

BEZEL_TEST(choice_match_35_5,
           spec_35_5_payload_case_bare_error_names_discard_remedy) {
    // §35.5 diagnostic: the error names the explicit discard remedy
    // (`Some(_)`), so a user knows the visible form for "matched, payload
    // ignored." Pins the message wording per §35.5 "Case patterns".
    auto r = semaAll(
        "module M; choice Opt Some(value: int), None; end Opt; "
        "class C public static method Main(): int "
        "  var x: Opt := Opt.None; "
        "  case x of "
        "    Some      => do return 1; end, "
        "    None      => do return 0; end "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "Some(_)"));
}

// --- §35.5 arity-mismatch rejection ----------------------------------------

BEZEL_TEST(choice_match_35_5,
           spec_35_5_payload_case_arity_under_bound_rejected) {
    // §35.5: a payload case is matched by a positional destructuring
    // pattern "with the case's exact payload arity." An under-bound
    // pattern is a compile error.
    auto r = semaAll(
        "module M; choice Pair First(a: int, b: int), Zero; end Pair; "
        "class C public static method Main(): int "
        "  var x: Pair := Pair.Zero; "
        "  case x of "
        "    First(var a) => do return a; end, "
        "    Zero          => do return 0; end "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "case-destructuring pattern"));
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "2 payload field"));
}

BEZEL_TEST(choice_match_35_5,
           spec_35_5_payload_case_arity_over_bound_rejected) {
    // §35.5: an over-bound pattern is likewise a compile error (mirrors
    // the record-destructuring §19.3 arity check).
    auto r = semaAll(
        "module M; choice Opt Some(value: int), None; end Opt; "
        "class C public static method Main(): int "
        "  var x: Opt := Opt.None; "
        "  case x of "
        "    Some(var a, var b) => do return a + b; end, "
        "    None               => do return 0; end "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "case-destructuring pattern"));
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "1 payload field"));
}

// --- §35.5: case names usable in arm bodies (prefix-omission) --------------

BEZEL_TEST(choice_match_35_5,
           spec_35_5_payload_free_case_name_usable_in_arm_body) {
    // §35.5 "Case patterns" (extends §10.6 to choices): the scrutinee
    // type's prefix may be omitted throughout the construct, INCLUDING in
    // arm bodies. A bare `None` in an arm body resolves to the case.
    // (Stage 1 set up qualified case access `Opt.None`; this test pins
    // that the bare form also types correctly inside a choice match.)
    auto r = semaAll(
        "module M; choice Opt Some(value: int), None; end Opt; "
        "class C public static method Main(): int "
        "  var x: Opt := Opt.None; "
        "  var hit: int := 0; "
        "  case x of "
        "    Some(var v) => do hit := 1; end, "
        "    None        => do var y: Opt := None; hit := 2; end "
        "  end; "
        "  return hit; "
        "end Main; end C;");
    // The bare `None` in the arm body resolved (no "does not name a type"
    // / unresolved-identifier error for it).
    BEZEL_EXPECT_TRUE(lacksErrDiagWith(r, "does not name a type"));
}
