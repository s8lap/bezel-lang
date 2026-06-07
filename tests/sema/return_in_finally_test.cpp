// Spec-derived tests for §31.2: `return` inside a `finally` block is a
// compile error. Appendix A grammar note: "`return` does not appear in
// `finally` blocks."
//
// Bug #470: the compiler currently accepts `return` inside `finally` and
// emits no diagnostic. These tests verify the fix.

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

// Convenience: wrap a body inside a static int method.
std::string wrapIntMethod(const std::string& body) {
    return "module M; class C "
           "public static method Foo(): int "
           + body +
           " end Foo; "
           "public static method Main(): int => Foo(); "
           "end C;";
}

// Convenience: wrap a body inside a static void method.
std::string wrapVoidMethod(const std::string& body) {
    return "module M; class C "
           "public static method Foo() "
           + body +
           " end Foo; "
           "end C;";
}

} // namespace

// §31.2: `return` inside a `finally` block is a compile error.
// Repro from bug #470: the compiler silently accepted this.
BEZEL_TEST(return_in_finally_470, return_in_finally_rejected_spec_31_2) {
    auto r = parseSemaAll(wrapIntMethod(
        "try var x := 1; "
        "finally return 42; "
        "end; "
        "return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §31.2: `return` with a value inside `finally` — also rejected.
BEZEL_TEST(return_in_finally_470, value_return_in_finally_rejected_spec_31_2) {
    auto r = parseSemaAll(wrapIntMethod(
        "try var x := 1; "
        "finally return 1 + 1; "
        "end; "
        "return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §31.2: `return` in `finally` of a void method — also rejected.
BEZEL_TEST(return_in_finally_470, bare_return_in_finally_void_method_rejected_spec_31_2) {
    auto r = parseSemaAll(wrapVoidMethod(
        "try var x := 1; "
        "finally return; "
        "end; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §31.2: `return` inside a nested block within `finally` is still rejected.
BEZEL_TEST(return_in_finally_470, return_in_nested_if_inside_finally_rejected_spec_31_2) {
    auto r = parseSemaAll(wrapIntMethod(
        "try var x := 1; "
        "finally "
        "  if true then return 7; end; "
        "end; "
        "return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Positive: `return` in a try *body* (not finally) is OK.
BEZEL_TEST(return_in_finally_470, return_in_try_body_ok_spec_31_2) {
    auto r = parseSemaAll(wrapIntMethod(
        "try return 42; "
        "finally var x := 1; "
        "end; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Positive: `return` in a catch arm body (not finally) is OK.
BEZEL_TEST(return_in_finally_470, return_in_catch_body_ok_spec_31_2) {
    auto r = parseSemaAll(wrapIntMethod(
        "try var x := 1; "
        "catch Exception => return 99; "
        "end; "
        "return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Positive: `return` outside a try/finally is always OK.
BEZEL_TEST(return_in_finally_470, return_outside_finally_ok_spec_31_2) {
    auto r = parseSemaAll(wrapIntMethod("return 5; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Positive: try/finally without any `return` in finally is OK.
BEZEL_TEST(return_in_finally_470, no_return_in_finally_ok_spec_31_2) {
    auto r = parseSemaAll(wrapIntMethod(
        "try var x := 1; "
        "finally var y := 2; "
        "end; "
        "return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Positive: `return` in a method that has a try/catch/finally but no `return`
// inside the finally block itself is OK.
BEZEL_TEST(return_in_finally_470, return_after_try_catch_finally_ok_spec_31_2) {
    auto r = parseSemaAll(wrapIntMethod(
        "try var x := 1; "
        "catch Exception => var y := 0; "
        "finally var z := 0; "
        "end; "
        "return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
