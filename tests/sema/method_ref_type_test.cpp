// Spec-derived tests for §27.6 + §5.1 (issue #436): a bare method reference
// `obj.Method` (no call parens) used in value position has the function type
// `(paramTypes) -> returnType`. When this function type is not assignable to
// the target type (§5.1), sema emits a clean compile error instead of
// allowing malformed IR to be generated.

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

// Helper: class A with `public method GetVal(): int => 42;`
// and class C with a static method that takes `a: A`.
std::string withA(const std::string& body) {
    return "module M; "
           "class A public method GetVal(): int => 42; end A; "
           "class C public static method Foo(a: A): int " +
           body +
           " end Foo; end C;";
}

} // namespace

// ── Rejection tests (§27.6 / §5.1 / issue #436) ─────────────────────────────
// A bare `obj.Method` has type `(params) -> returnType` and must NOT be
// implicitly convertible to a non-function type.

// §27.6 + §5.1 example 1: `return a.GetVal;` where return type is int.
// The method reference `a.GetVal` has type `() -> int`; that is NOT an int.
// This must produce a compile error — not silent malformed IR.
BEZEL_TEST(sema_method_ref_type, return_bare_method_ref_wrong_type_diagnoses) {
    auto r = parseSemaAll(withA("return a.GetVal;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §27.6 + §5.1 example 2: `var x: int := a.GetVal;` — same mismatch.
// Assigning a `() -> int` to an `int` local is not assignable.
BEZEL_TEST(sema_method_ref_type, local_init_bare_method_ref_wrong_type_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class A public method GetVal(): int => 42; end A; "
        "class C public static method Foo(a: A) "
        "  var x: int := a.GetVal; "
        "end Foo; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §27.6 + §5.1 example 3: passing `a.GetVal` where an `int` parameter is
// expected. The function type `() -> int` is not assignable to `int`.
// (Argument type checking — tested via return to keep the repro simple.)
BEZEL_TEST(sema_method_ref_type, assign_bare_method_ref_to_int_local_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class A public method GetVal(): int => 42; end A; "
        "class C public static method Foo(a: A) "
        "  var x: int := 0; "
        "  x := a.GetVal; "
        "end Foo; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ── Control tests (must still compile cleanly) ────────────────────────────────
// §27.6: these are the CORRECT uses of method references as function values.

// §27.6 control 1: `var f: () -> int := a.GetVal;` — assigns a method reference
// to a function-typed local. This is the canonical idiom and must NOT error.
BEZEL_TEST(sema_method_ref_type, var_func_typed_local_from_method_ref_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class A public method GetVal(): int => 42; end A; "
        "class C public static method Foo(a: A): int "
        "  var f: () -> int := a.GetVal; "
        "  return f(); "
        "end Foo; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §27.6 control 2: `a.GetVal()` (called) — must NOT error; returns int.
BEZEL_TEST(sema_method_ref_type, called_method_ref_ok) {
    auto r = parseSemaAll(withA("return a.GetVal();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §27.6 control 3: static method reference passed where `(int) -> int` is
// expected. This is the existing method-reference codegen path; must not
// be broken by the new function-type check.
BEZEL_TEST(sema_method_ref_type, static_method_ref_passed_as_func_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class H public static method Square(x: int): int => x * x; end H; "
        "class C "
        "  public static method Apply(f: (int) -> int, n: int): int "
        "    return f(n); "
        "  end Apply; "
        "  public static method Main(): int "
        "    return Apply(H.Square, 5); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §27.6 control 4: method reference stored in a correctly-typed local,
// then passed where the same function type is expected.
BEZEL_TEST(sema_method_ref_type, method_ref_stored_in_func_typed_local_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class H public static method Negate(x: int): int => -x; end H; "
        "class C "
        "  public static method Apply(f: (int) -> int, n: int): int "
        "    return f(n); "
        "  end Apply; "
        "  public static method Main(): int "
        "    var neg: (int) -> int := H.Negate; "
        "    return Apply(neg, 7); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
