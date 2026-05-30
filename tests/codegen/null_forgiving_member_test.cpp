// Regression for #206 — member access through the postfix `!` operator.
// `x!.member` must read and write the underlying field; before the fix
// the null-forgiving expression had no recorded type, so `x!.V` resolved
// to nothing (reads returned garbage, writes were silently dropped).
// The `!` still raises NullReferenceException on an actually-null value
// (covered by CG23); here the operand is always non-null. Observable via
// the int exit code.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& body) {
    auto r = runJit(
        std::string("module M; class T public V: int := 0; end T; "
                    "class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_bang_member, read_through_bang) {
    BEZEL_EXPECT_EQ(run(
        "var x := new T(); x.V := 5; "
        "var a: T? := x; "
        "return a!.V;"), 5);
}

BEZEL_TEST(codegen_bang_member, write_through_bang) {
    BEZEL_EXPECT_EQ(run(
        "var x := new T(); "
        "var a: T? := x; "
        "a!.V := 7; "
        "return a!.V;"), 7);
}

BEZEL_TEST(codegen_bang_member, write_visible_via_original) {
    // Writing through `a!` mutates the same object `x` aliases.
    BEZEL_EXPECT_EQ(run(
        "var x := new T(); "
        "var a: T? := x; "
        "a!.V := 42; "
        "return x.V;"), 42);
}

BEZEL_TEST(codegen_bang_member, compound_assign_through_bang) {
    BEZEL_EXPECT_EQ(run(
        "var x := new T(); x.V := 40; "
        "var a: T? := x; "
        "a!.V += 2; "
        "return a!.V;"), 42);
}
