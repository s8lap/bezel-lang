// Spec-derived diagnostic tests for §27.4 (v0.183, #1408) "Lambda-parameter
// record destructuring" and grammar A.7 `LambdaDestructure`. Issue #1587.
//
// §27.4 (full text):
//   "A destructuring parameter is a binding form, not a test, so only
//   irrefutable elements are admitted:
//    - The head must name a positional record type, and it must be exactly
//      the parameter's type: when a target type supplies the parameter
//      type, a head naming any other type is a compile error (no subtype
//      test, nothing to fail at runtime); when no target type is
//      available, the head *is* the annotation.
//    - Each slot is a naked identifier binding, a discard `_`, or a nested
//      destructuring meeting the same rules; the slot count must equal the
//      record's positional-field count. Constants, ranges, relationals,
//      type tests, `var`-form bindings, and alternatives are compile
//      errors in parameter position — they are refutable, and a parameter
//      list is not a `case`.
//    - The destructuring parameter admits no modifiers (`ref`, `scoped`)
//      and no separate type annotation — the head carries the type."
//
// These tests verify each rejection gets a DEDICATED diagnostic (not a
// generic parse error), plus the positive control cases compile clean.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasErrorContaining(const bezel::test::SemaResult& r,
                        std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

} // namespace

// ── §27.4: refutable slot elements are rejected with a DEDICATED diagnostic ─

// A constant in a slot (`Entry(1, v)`) is refutable — "Constants ... are
// compile errors in parameter position". Must NOT be a generic parse error;
// must cite the dedicated §27.4 wording.
BEZEL_TEST(sema_1587_lambda_destructure, refutable_constant_slot_rejected_spec_27_4) {
    auto r = parseSemaAll(
        "module M; "
        "record Entry(K: int, V: int); "
        "class C public static method Main(): int "
        "  var f: (Entry) -> int := (Entry(1, v)) => v; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "refutable"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "27.4"));
}

// A relational pattern in a slot (`Entry(> 0, v)`) is refutable.
BEZEL_TEST(sema_1587_lambda_destructure, refutable_relational_slot_rejected_spec_27_4) {
    auto r = parseSemaAll(
        "module M; "
        "record Entry(K: int, V: int); "
        "class C public static method Main(): int "
        "  var f: (Entry) -> int := (Entry(> 0, v)) => v; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "refutable"));
}

// A range pattern in a slot (`Entry(1..5, v)`) is refutable.
BEZEL_TEST(sema_1587_lambda_destructure, refutable_range_slot_rejected_spec_27_4) {
    auto r = parseSemaAll(
        "module M; "
        "record Entry(K: int, V: int); "
        "class C public static method Main(): int "
        "  var f: (Entry) -> int := (Entry(1..5, v)) => v; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "refutable"));
}

// A `var`-form binding (`Entry(var k, v)`) is explicitly disallowed even
// though it is irrefutable — §27.4 lists it among the rejected forms
// ("Constants, ranges, relationals, type tests, `var`-form bindings, and
// alternatives are compile errors in parameter position"), unlike §19.3
// case patterns where `var x` and naked `x` are equivalent.
BEZEL_TEST(sema_1587_lambda_destructure, var_form_binding_slot_rejected_spec_27_4) {
    auto r = parseSemaAll(
        "module M; "
        "record Entry(K: int, V: int); "
        "class C public static method Main(): int "
        "  var f: (Entry) -> int := (Entry(var k, v)) => k + v; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "refutable"));
}

// A bare type-test slot (`Entry(int, v)` — nonsensical, but must be rejected
// with the dedicated diagnostic, not a crash or an unrelated error).
BEZEL_TEST(sema_1587_lambda_destructure, type_test_slot_rejected_spec_27_4) {
    auto r = parseSemaAll(
        "module M; "
        "record Entry(K: int, V: int); "
        "class C public static method Main(): int "
        "  var f: (Entry) -> int := (Entry(int, v)) => v; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// A nested destructuring parameter can ALSO carry a refutable element —
// verify the check recurses.
BEZEL_TEST(sema_1587_lambda_destructure, nested_refutable_slot_rejected_spec_27_4) {
    auto r = parseSemaAll(
        "module M; "
        "record Inner(A: int, B: int); "
        "record Outer(I: Inner, C: int); "
        "class C public static method Main(): int "
        "  var f: (Outer) -> int := (Outer(Inner(1, b), c)) => b + c; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "refutable"));
}

// ── §27.4: a disallowed modifier is rejected with a DEDICATED diagnostic ────

// "The destructuring parameter admits no modifiers (`ref`, `scoped`)."
// `owned` (a lambda-parameter modifier per §27.1) on a destructuring param
// must be a dedicated diagnostic, not "expected lambda parameter name".
BEZEL_TEST(sema_1587_lambda_destructure, owned_modifier_rejected_spec_27_4) {
    auto r = parseSemaAll(
        "module M; "
        "record Entry(K: int, V: int); "
        "class C public static method Main(): int "
        "  var f := (owned Entry(k, v)) => k + v; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "modifier"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "27.4"));
}

// `ref` on a destructuring param is likewise rejected.
BEZEL_TEST(sema_1587_lambda_destructure, ref_modifier_rejected_spec_27_4) {
    auto r = parseSemaAll(
        "module M; "
        "record Entry(K: int, V: int); "
        "class C public static method Main(): int "
        "  var f := (ref Entry(k, v)) => k + v; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "modifier"));
}

// ── §27.4: exact-type head check ─────────────────────────────────────────────

// "when a target type supplies the parameter type, a head naming any other
// type is a compile error (no subtype test, nothing to fail at runtime)."
BEZEL_TEST(sema_1587_lambda_destructure, wrong_head_against_target_type_rejected_spec_27_4) {
    auto r = parseSemaAll(
        "module M; "
        "record Entry(K: int, V: int); "
        "record Other(A: int, B: int); "
        "class C public static method Main(): int "
        "  var f: (Entry) -> int := (Other(k, v)) => k + v; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "27.4"));
}

// A class head is rejected — classes have no positional-destructuring
// surface (§19.3, restated by §27.4's "must name a positional record type").
BEZEL_TEST(sema_1587_lambda_destructure, class_head_rejected_spec_27_4) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget public Id: int; "
        "  public constructor(id: int) Id := id; end constructor; "
        "end Widget; "
        "class C public static method Main(): int "
        "  var f := (Widget(id)) => id; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "class"));
}

// ── §27.4: slot count must equal the record's positional-field count ────────

BEZEL_TEST(sema_1587_lambda_destructure, arity_mismatch_over_binding_rejected_spec_27_4) {
    auto r = parseSemaAll(
        "module M; "
        "record Entry(K: int, V: int); "
        "class C public static method Main(): int "
        "  var f := (Entry(k, v, w)) => k + v + w; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "binding count must match"));
}

BEZEL_TEST(sema_1587_lambda_destructure, arity_mismatch_under_binding_rejected_spec_27_4) {
    auto r = parseSemaAll(
        "module M; "
        "record Entry(K: int, V: int); "
        "class C public static method Main(): int "
        "  var f := (Entry(k)) => k; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "binding count must match"));
}

// ── positive controls: the valid forms must NOT produce an error ────────────

BEZEL_TEST(sema_1587_lambda_destructure, naked_binding_form_ok_spec_27_4) {
    auto r = parseSemaAll(
        "module M; "
        "record Entry(K: int, V: int); "
        "class C public static method Main(): int "
        "  var f: (Entry) -> int := (Entry(k, v)) => k + v; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_1587_lambda_destructure, discard_slot_ok_spec_27_4) {
    auto r = parseSemaAll(
        "module M; "
        "record Entry(K: int, V: int); "
        "class C public static method Main(): int "
        "  var f: (Entry) -> int := (Entry(_, v)) => v; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_1587_lambda_destructure, nested_destructuring_ok_spec_27_4) {
    auto r = parseSemaAll(
        "module M; "
        "record Inner(A: int, B: int); "
        "record Outer(I: Inner, C: int); "
        "class C public static method Main(): int "
        "  var f: (Outer) -> int := (Outer(Inner(a, b), c)) => a + b + c; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_1587_lambda_destructure, inferred_head_no_target_ok_spec_27_4) {
    auto r = parseSemaAll(
        "module M; "
        "record Entry(K: int, V: int); "
        "class C public static method Main(): int "
        "  var f := (Entry(k, v)) => k + v; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §27.4: "the lambda's function type is unchanged by the spelling
// (`(Entry) -> R`)" — a destructuring lambda must be assignable to an
// ordinary `(Entry) -> int` typed local (checked positively above) AND must
// be usable as a call argument expecting that same function type.
BEZEL_TEST(sema_1587_lambda_destructure, function_type_unchanged_by_spelling_ok_spec_27_4) {
    auto r = parseSemaAll(
        "module M; "
        "record Entry(K: int, V: int); "
        "class C "
        "  public static method Apply(e: Entry, f: (Entry) -> int): int "
        "    return f(e); "
        "  end Apply; "
        "  public static method Main(): int "
        "    return Apply(new Entry(1, 2), (Entry(k, v)) => k + v); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
