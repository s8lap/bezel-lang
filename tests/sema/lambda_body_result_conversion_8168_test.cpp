// fix #8168 (§27.3 *Admission at a target-typed non-call position*, §5.1,
// v0.355): a lambda literal at a function-typed target is typed, "and its
// inferred return type must equal the target's return slot or implicitly
// convert to it (§5.1); otherwise the lambda is a compile error at the
// position". None of the six result pairs below is a row of §5.1's table
// (`double` converts to no other numeric type; `decimal` to none; `float`
// only to `double`; `int` to `long`, `float`, `double`, `decimal`; `long` to
// `float`, `double`, `decimal`), and two of them fall under §5.1's *never
// implicit* bullets ("Between signed and unsigned types of the same or
// narrower width", "Between `decimal` and binary floats"). Before the fix
// every one compiled (rc 0): `(double) -> float` printed `0` and
// `(decimal) -> double` SEGV'd in the closure.
//
// The admitting twins (legal §5.1 widenings, and §5.1 constant narrowing of
// an integer constant body) stay admitted: see
// tests/codegen/lambda_body_result_widening_8168_test.cpp for their output.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;
using bezel::test::diagnosticMessageContains;

namespace {

const char* kNeedle8168 =
    "lambda body's returned value has a type that is not implicitly "
    "convertible to the target type (§5.1)";

std::string prog8168(const std::string& body) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n" +
           body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

void expectRejected8168(const std::string& body) {
    auto r = runAot(prog8168(body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNeedle8168));
}

}  // namespace

// The issue's p1.bzl first line: `double` -> `float` is not a §5.1 row.
BEZEL_TEST(lambda_body_result_conversion_8168,
           double_body_at_float_slot_rejected_spec_27_3) {
    expectRejected8168(
        "    var g: (double) -> float := lambda(x: double) => x;\n"
        "    Console.WriteLine(g(1.5));\n");
}

// `decimal` -> `double`: §5.1 "never implicit ... Between `decimal` and
// binary floats". Pre-fix this SEGV'd in `lam$1`.
BEZEL_TEST(lambda_body_result_conversion_8168,
           decimal_body_at_double_slot_rejected_spec_27_3) {
    expectRejected8168(
        "    var h: (decimal) -> double := lambda(x: decimal) => x;\n"
        "    Console.WriteLine(h(2.5m));\n");
}

// `float` -> `decimal`: the same never-implicit bullet, other direction.
BEZEL_TEST(lambda_body_result_conversion_8168,
           float_body_at_decimal_slot_rejected_spec_27_3) {
    expectRejected8168(
        "    var a: (float) -> decimal := lambda(x: float) => x;\n"
        "    Console.WriteLine(a(1.5f));\n");
}

// `int` -> `ulong`: §5.1 "never implicit ... Between signed and unsigned
// types" (`int`'s row lists `long`, `float`, `double`, `decimal`, no `ulong`).
BEZEL_TEST(lambda_body_result_conversion_8168,
           int_body_at_ulong_slot_rejected_spec_27_3) {
    expectRejected8168(
        "    var b: (int) -> ulong := lambda(x: int) => x;\n"
        "    Console.WriteLine(b(3));\n");
}

// `long` -> `int`: a narrowing; the body `x` is not a constant.
BEZEL_TEST(lambda_body_result_conversion_8168,
           long_body_at_int_slot_rejected_spec_27_3) {
    expectRejected8168(
        "    var c: (long) -> int := lambda(x: long) => x;\n"
        "    Console.WriteLine(c(3L));\n");
}

// `double` -> `decimal`: §5.1's decimal/binary-float bullet.
BEZEL_TEST(lambda_body_result_conversion_8168,
           double_body_at_decimal_slot_rejected_spec_27_3) {
    expectRejected8168(
        "    var e: (double) -> decimal := lambda(x: double) => x;\n"
        "    Console.WriteLine(e(1.5));\n");
}

// The other spellings of the same position: an UNANNOTATED parameter (its
// type is the target's, §27.4, so the body is `long`), a BLOCK body's
// `return x;`, and an assignment to a function-typed location.
BEZEL_TEST(lambda_body_result_conversion_8168,
           unannotated_param_body_at_narrower_slot_rejected_spec_27_3) {
    expectRejected8168(
        "    var c: (long) -> int := lambda(x) => x;\n"
        "    Console.WriteLine(c(3L));\n");
}

BEZEL_TEST(lambda_body_result_conversion_8168,
           block_body_return_at_narrower_slot_rejected_spec_27_3) {
    expectRejected8168(
        "    var c: (long) -> int := lambda(x: long) => do return x; end;\n"
        "    Console.WriteLine(c(3L));\n");
}

BEZEL_TEST(lambda_body_result_conversion_8168,
           assignment_target_lambda_at_narrower_slot_rejected_spec_27_3) {
    expectRejected8168(
        "    var c: (long) -> int := lambda(x: long) => 1;\n"
        "    c := lambda(x: long) => x;\n"
        "    Console.WriteLine(c(3L));\n");
}

// A non-numeric pair through the same predicate: an `int` body at a `string`
// slot (pre-fix: an IR-verification codegen error, not a diagnostic).
BEZEL_TEST(lambda_body_result_conversion_8168,
           int_body_at_string_slot_rejected_spec_27_3) {
    expectRejected8168(
        "    var c: (int) -> string := lambda(x: int) => x;\n"
        "    Console.WriteLine(c(3));\n");
}

// narrow #8168 (second slice): the return slot is a declared NON-primitive
// type (class, record, enum). §27.3 applies the same sentence ("its inferred
// return type must equal the target's return slot or implicitly convert to
// it (§5.1)"); pre-fix `(int) -> Item := lambda(x: int) => x` failed IR
// verification and `() -> Point := lambda() => new Item()` printed garbage.
namespace {
const char* kDecls8168 =
    "class Item\n"
    "  public method V(): int => 7;\n"
    "end Item;\n"
    "record Point(X: int, Y: int);\n"
    "enum Pri\n"
    "  Low, High;\n"
    "end Pri;\n";

void expectRejectedDecl8168(const std::string& body) {
    auto r = runAot(std::string("module M;\n") + kDecls8168 +
                    "class C\n"
                    "  public static method Main(): int\n" +
                    body +
                    "    return 0;\n"
                    "  end Main;\n"
                    "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNeedle8168));
}
}  // namespace

BEZEL_TEST(lambda_body_result_conversion_8168,
           int_body_at_class_slot_rejected_spec_27_3) {
    expectRejectedDecl8168(
        "    var f: (int) -> Item := lambda(x: int) => x;\n"
        "    Console.WriteLine(f(1).V());\n");
}

BEZEL_TEST(lambda_body_result_conversion_8168,
           string_body_at_class_slot_rejected_spec_27_3) {
    expectRejectedDecl8168(
        "    var f: (int) -> Item := lambda(x) => \"s\";\n"
        "    Console.WriteLine(f(1).V());\n");
}

BEZEL_TEST(lambda_body_result_conversion_8168,
           class_body_at_record_slot_rejected_spec_27_3) {
    expectRejectedDecl8168(
        "    var f: () -> Point := lambda() => new Item();\n"
        "    Console.WriteLine(f().X);\n");
}

BEZEL_TEST(lambda_body_result_conversion_8168,
           int_body_at_enum_slot_rejected_spec_27_3) {
    expectRejectedDecl8168(
        "    var f: () -> Pri := lambda() => 1;\n"
        "    Console.WriteLine(f() = Pri.High);\n");
}

BEZEL_TEST(lambda_body_result_conversion_8168,
           block_body_int_return_at_class_slot_rejected_spec_27_3) {
    expectRejectedDecl8168(
        "    var f: (int) -> Item := lambda(x: int) => do return x; end;\n"
        "    Console.WriteLine(f(1).V());\n");
}
