// Codegen slice CG44 — explicit conversions `T(x)` (§5.2). A primitive type
// name applied with call syntax converts its argument: integer width
// changes (trunc / sext / zext), int↔float (si/ui<->fp), and float↔double
// (fpext / fptrunc). Narrowing overflow checks are deferred. Observable via
// the int exit code.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {
int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}
} // namespace

BEZEL_TEST(codegen_cg44, char_to_int) {
    BEZEL_EXPECT_EQ(run("var c := 'A'; return int(c) + 1;"), 66);
}

BEZEL_TEST(codegen_cg44, double_to_int_truncates) {
    BEZEL_EXPECT_EQ(run("var d := 3.9; return int(d);"), 3);
}

BEZEL_TEST(codegen_cg44, int_to_double) {
    BEZEL_EXPECT_EQ(run("var n := 7; var d := double(n); "
                        "return (1 if d = 7.0 else 0);"), 1);
}

BEZEL_TEST(codegen_cg44, long_to_int) {
    // 300 does not fit an 8-bit exit code, so this builds the full program
    // directly and observes stdout (run()'s body-only wrapper can't route
    // through runAotOut).
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "var x := 300L; Console.WriteLine(int(x)); return 0; "
        " end Main; end C;"), "300\n");
}

BEZEL_TEST(codegen_cg44, int_to_byte_in_range) {
    BEZEL_EXPECT_EQ(run("var n := 200; var b := byte(n); return int(b);"), 200);
}

// §5.2: integer narrowing raises OverflowException when the value does
// not fit the target type.
BEZEL_TEST(codegen_cg44, int_to_byte_overflow_raises) {
    BEZEL_EXPECT_EQ(run(
        "var n := 258; "
        "try var b := byte(n); return int(b); "
        "catch OverflowException => return 99; end;"), 99);
}

BEZEL_TEST(codegen_cg44, int_to_short_overflow_raises) {
    BEZEL_EXPECT_EQ(run(
        "var n := 70000; "
        "try var s := short(n); return int(s); "
        "catch OverflowException => return 77; end;"), 77);
}

BEZEL_TEST(codegen_cg44, signed_narrowing_overflow_raises) {
    // 200 does not fit a signed sbyte (range −128..127).
    BEZEL_EXPECT_EQ(run(
        "var n := 200; "
        "try var b := sbyte(n); return int(b); "
        "catch OverflowException => return 88; end;"), 88);
}

BEZEL_TEST(codegen_cg44, int_to_long_widens) {
    BEZEL_EXPECT_EQ(run("var n := 5; var x := long(n); return int(x);"), 5);
}

// -3 does not fit an 8-bit exit code — observe on stdout.
BEZEL_TEST(codegen_cg44, negative_int_to_long_sign_extends) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "var n := -3; var x := long(n); Console.WriteLine(int(x)); return 0; "
        " end Main; end C;"), "-3\n");
}

BEZEL_TEST(codegen_cg44, conversion_in_arithmetic) {
    BEZEL_EXPECT_EQ(run("var d := 2.5; return int(d) * 20 + 2;"), 42);
}

// §5.4 enum casts: `int(E.X)` is the constant's value; `E(i)` builds an
// enum from an integer (the enum is represented by its underlying i32).
BEZEL_TEST(codegen_cg44, int_of_enum_constant) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; enum E Red, Green, Blue; end E; "
        "class C public static method Main(): int return int(E.Blue); "
        "end Main; end C;").exitCode, 2);
}

BEZEL_TEST(codegen_cg44, enum_of_int_roundtrips_through_case) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; enum E Red, Green, Blue; end E; "
        "class C public static method Main(): int "
        "  var e := E(2); return case e of E.Blue => 9, _ => 0, end; "
        "end Main; end C;").exitCode, 9);
}

BEZEL_TEST(codegen_cg44, enum_of_int_then_back_to_int) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; enum E Red, Green, Blue; end E; "
        "class C public static method Main(): int "
        "  var e := E(1); return int(e); "
        "end Main; end C;").exitCode, 1);
}

BEZEL_TEST(codegen_cg44, int_of_enum_with_explicit_values) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; enum E A := 10, B := 20; end E; "
        "class C public static method Main(): int return int(E.B); "
        "end Main; end C;").exitCode, 20);
}

// NOTE (#1322 / §5.1/§5.2): the four `bool(n)` runtime tests added by #1269
// have been migrated to sema/bool_int_conversion_1322_test.cpp as compile-time
// reject tests.  The spec (§5.1: "Between integers and `bool`" is never
// implicit; §5.2: does not list `bool` as an explicit-conversion target)
// forbids the conversion entirely; it must be caught at sema, not at runtime.
