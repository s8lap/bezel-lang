// Regression tests for issue #6310 (sema, spec §27.6, v0.338).
//
// A bare overloaded method group as an expression statement (`Math.Abs;`)
// compiled with no diagnostic. An expression statement has no target function
// type, and §27.6 "Method references to overloaded methods" states that when no
// target type is available the reference is ambiguous and the compiler reports
// an error. The var-initializer form `var f := Math.Abs;` already reports this;
// the statement form now reports the same diagnostic.
//
// Controls: a call statement, a non-overloaded method group, a field read and
// every other statement form the probe matrix covers stay accepted (§21.9's
// "any expression" statement form; the discarded value is not checked here).
//
// per spec §27.6 (v0.338), §21.9 (v0.338).

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

// Wraps `body` in a class with an instance field `x`, a non-overloaded instance
// method `Inst1`, an overloaded instance method `Ov`, an overloaded static
// method `SOv`, a non-overloaded static method `Stat`, and returns `0` after
// `body`. Each program's exit value is decided by its own `body`.
std::string program(const std::string& body) {
    return "module M; class C "
           "public x: int := 3; "
           "public method Inst1(a: int): int return a; end Inst1; "
           "public method Ov(a: int): int return a; end Ov; "
           "public method Ov(a: long): long return a; end Ov; "
           "public static method SOv(a: int): int return a; end SOv; "
           "public static method SOv(a: long): long return a; end SOv; "
           "public static method Stat(a: int): int return a; end Stat; "
           "public static method Main(): int " +
           body + " return 0; end Main; end C;";
}

// The statement must fail at compile time with a located diagnostic carrying
// `needle` exactly.
bool rejectedWith(const std::string& body, const char* needle) {
    auto r = runAot(program(body));
    return bezel::test::failedAtCompileTimeWithRealLocation(r) &&
           bezel::test::diagnosticMessageContains(r, needle);
}

}  // namespace

// ---------------------------------------------------------------------------
// REJECT: the issue repro and its sibling method groups, exact diagnostic.
// ---------------------------------------------------------------------------

BEZEL_TEST(method_group_statement_6310, math_abs_bare_statement_rejected) {
    // The issue repro: `Math.Abs;` as an expression statement.
    BEZEL_EXPECT_TRUE(rejectedWith(
        "Math.Abs; ",
        "the method reference `Math.Abs` is ambiguous: it is overloaded and has "
        "no target type to select by (\xC2\xA7""27.6, v0.338)"));
}

BEZEL_TEST(method_group_statement_6310, math_abs_bare_statement_after_statements_rejected) {
    // The same statement later in a body, after other statements.
    BEZEL_EXPECT_TRUE(rejectedWith(
        "var a: int := 1; var b: int := 2; Math.Abs; return a + b; ",
        "the method reference `Math.Abs` is ambiguous: it is overloaded and has "
        "no target type to select by (\xC2\xA7""27.6, v0.338)"));
}

BEZEL_TEST(method_group_statement_6310, overloaded_instance_group_statement_rejected) {
    // Sibling: an overloaded instance method group as a bare statement.
    BEZEL_EXPECT_TRUE(rejectedWith(
        "var c: C := new C(); c.Ov; ",
        "the method reference `Ov` is overloaded and has no target type to select by "
        "(\xC2\xA7""27.6 v0.307)"));
}

BEZEL_TEST(method_group_statement_6310, overloaded_static_group_statement_rejected) {
    // Sibling: an overloaded static method group as a bare statement.
    BEZEL_EXPECT_TRUE(rejectedWith(
        "C.SOv; ",
        "the method reference `SOv` is overloaded and has no target type to select by "
        "(\xC2\xA7""27.6 v0.307)"));
}

// ---------------------------------------------------------------------------
// ACCEPT controls: every other statement form stays accepted and runs.
// ---------------------------------------------------------------------------

BEZEL_TEST(method_group_statement_6310, call_statements_accepted) {
    // A call is a call, not a method group: `Math.Abs(-1);` and `Stat(a);`.
    auto r = runAot(program(
        "var a: int := 4; Math.Abs(-1); Stat(a); return a; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);
}

BEZEL_TEST(method_group_statement_6310, annotated_overloaded_reference_accepted) {
    // The target-typed reference `var f: (int) -> int := Math.Abs;` is the
    // spec's disambiguation and stays accepted.
    auto r = runAot(program(
        "var f: (int) -> int := Math.Abs; return f(-7); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

BEZEL_TEST(method_group_statement_6310, non_overloaded_group_statement_accepted) {
    // A non-overloaded method group is not the ambiguous case; base accepts it.
    auto r = runAot(program(
        "var c: C := new C(); C.Stat; c.Inst1; return 5; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

BEZEL_TEST(method_group_statement_6310, value_expression_statements_accepted) {
    // The other bare expression forms the probe matrix covers: a local read,
    // an integer literal, an additive expression, a parenthesised call, a
    // field read, a `new` expression, and the increment/assignment statements.
    auto r = runAot(program(
        "var a: int := 1; var b: int := 2; var i: int := 0; "
        "a; 5; a + b; (Stat(a)); "
        "var c: C := new C(); c.x; new C(); "
        "i++; i += 2; i := i + 1; "
        "return i; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);
}
