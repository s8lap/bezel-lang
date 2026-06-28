// Spec-derived tests for issue #1126 — `Object.ReferenceEquals(a, b)` on a
// VALUE-TYPE argument must emit the §8.4 advisory WARNING.
//
// Spec basis (§8.4): "`Object.ReferenceEquals(a, b)` compares object identity,
// ignoring any `Equals` override. Passing value-type arguments always returns
// `false` (each boxing creates a fresh object); the compiler warns."
//
// The result is already correct (#1119 lowers a pointer-identity compare, which
// yields `false` for two freshly-boxed values). This pass adds the advisory
// diagnostic the spec mandates. It is a WARNING, not an error: the program still
// compiles and runs. A value type is a non-`string`/non-`Object` primitive, a
// record, or an enum (§14.2 / §3); classes, interfaces, builtin reference types
// and `string` are reference types and must NOT warn.

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
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

// True iff some emitted diagnostic is the #1126 §8.4 advisory: a WARNING whose
// message names `Object.ReferenceEquals`, the value-type cause, and §8.4.
bool hasValueTypeRefEqualsWarning(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("Object.ReferenceEquals") == std::string::npos)
            continue;
        if (d.message.find("value-type") == std::string::npos) continue;
        if (d.message.find("\xC2\xA7""8.4") == std::string::npos) continue;
        return true;
    }
    return false;
}

} // namespace

// ===========================================================================
// POSITIVE — the §8.4 advisory must fire, and it is a WARNING (no errors).
// ===========================================================================

// §8.4 / §3: a primitive value-type argument (`int`) warns.
BEZEL_TEST(sema_reference_equals_value_warning_1126, primitive_int_arg_warns_1126) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    return (1 if Object.ReferenceEquals(5, 6) else 0); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());  // advisory only
    BEZEL_EXPECT_TRUE(hasValueTypeRefEqualsWarning(r));
}

// §8.4 / §9: a record value-type argument warns.
BEZEL_TEST(sema_reference_equals_value_warning_1126, record_arg_warns_1126) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(x: int, y: int); "
        "class C "
        "  public method M(p: Point, q: Point): bool "
        "    => Object.ReferenceEquals(p, q); "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasValueTypeRefEqualsWarning(r));
}

// §8.4 / §10: an enum value-type argument warns.
BEZEL_TEST(sema_reference_equals_value_warning_1126, enum_arg_warns_1126) {
    auto r = parseSemaAll(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "class C "
        "  public method M(a: Color, b: Color): bool "
        "    => Object.ReferenceEquals(a, b); "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasValueTypeRefEqualsWarning(r));
}

// §8.4: a MIXED call (one reference operand, one value operand) still warns —
// the value operand alone makes the comparison meaningless.
BEZEL_TEST(sema_reference_equals_value_warning_1126, mixed_ref_value_arg_warns_1126) {
    auto r = parseSemaAll(
        "module M; "
        "class N public v: int := 0; end N; "
        "class C "
        "  public method M(n: N): bool "
        "    => Object.ReferenceEquals(n, 7); "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasValueTypeRefEqualsWarning(r));
}

// ===========================================================================
// NEGATIVE — reference-type operands must NOT warn (must stay clean).
// ===========================================================================

// §8.4 / §14.2: two class (reference-type) operands — no warning.
BEZEL_TEST(sema_reference_equals_value_warning_1126, two_class_refs_no_warning_1126) {
    auto r = parseSemaAll(
        "module M; "
        "class N public v: int := 0; end N; "
        "class C "
        "  public method M(a: N, b: N): bool "
        "    => Object.ReferenceEquals(a, b); "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasValueTypeRefEqualsWarning(r));
}

// §8.4 / §14.2: `string` is a reference type — no warning.
BEZEL_TEST(sema_reference_equals_value_warning_1126, string_refs_no_warning_1126) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M(a: string, b: string): bool "
        "    => Object.ReferenceEquals(a, b); "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasValueTypeRefEqualsWarning(r));
}
