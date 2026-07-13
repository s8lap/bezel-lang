// §5.1 / §22.2 (issue #1774): an EXPRESSION-bodied method (`method Foo(): int
// => <expr>`) must run the SAME return-type convertibility check the block form
// `{ return <expr>; }` runs. §22.2 makes `=> E` spec-equivalent to returning E,
// so §5.1 governs whether E's type converts to the declared return type.
//
// Before this fix the arrow-body path in ExprTypecheck.cpp::runMethodBody
// visited the body expression but never checkAssignable'd it against the
// declared return type (the block-form ReturnStmt arm did, "Slice T3"). A
// type-mismatched arrow body therefore reached codegen and emitted malformed
// LLVM IR — a `ret` whose operand type differed from the function's return type
// ("Function return type does not match operand type of return inst! ret ptr /
// i32"), a verifier failure / miscompile. The examples from the bug:
//   method Pub(): int => "x"          (string is not convertible to int)
//   method Pub(): int => this.Helper  (a paren-less method reference has type
//                                       `() -> int`, a Function, not int)
// were both accepted; the BLOCK form `... return "x"; end` was correctly
// rejected with the §5.1 diagnostic. This test pins that the arrow form now
// rejects them too, and that a VALID arrow body still compiles clean.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// The §5.1 return-convertibility diagnostic (checkAssignable, what="returned
// value") — "... is not implicitly convertible to the target type (§5.1)".
bool hasReturnConvertError(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not implicitly convertible") != std::string::npos &&
            d.message.find("5.1") != std::string::npos)
            return true;
    return false;
}

bezel::test::OwnershipResult check(const std::string& src) {
    return parseAndCheckOwnership(src);
}

}  // namespace

// §5.1 / §22.2 (#1774): plain type mismatch — a `string` arrow body for an
// `int`-returning method is rejected, exactly as the block form is.
BEZEL_TEST(arrow_body_return_1774, string_body_int_return_rejected_spec_5_1) {
    auto r = check(
        "module M; "
        "class C "
        "  public method Pub(): int => \"x\"; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasReturnConvertError(r));
}

// §5.1 / §22.2 / §27.6 (#1774): a paren-less method reference (`this.Helper`,
// no call parens) has a Function type `() -> int`, which is not convertible to
// the declared `int` return. The other reproducer from the bug.
BEZEL_TEST(arrow_body_return_1774, method_ref_body_int_return_rejected_spec_5_1) {
    auto r = check(
        "module M; "
        "class C "
        "  public method Helper(): int => 1; "
        "  public method Pub(): int => this.Helper; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasReturnConvertError(r));
}

// §22.2 (#1774): the equivalent BLOCK form was ALREADY rejected — a sanity
// anchor showing the arrow form now matches the block form's behavior.
BEZEL_TEST(arrow_body_return_1774, block_form_string_int_return_rejected_spec_5_1) {
    auto r = check(
        "module M; "
        "class C "
        "  public method Pub(): int "
        "    return \"x\"; "
        "  end Pub; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasReturnConvertError(r));
}

// §5.1 (#1774): a VALID exact-type arrow body must still compile with no error
// — the new check must not over-reject a well-typed body.
BEZEL_TEST(arrow_body_return_1774, int_body_int_return_accepted) {
    auto r = check(
        "module M; "
        "class C "
        "  public method Pub(): int => 42; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §5.1 (#1774): a §5.1 IMPLICIT WIDENING (`int` constant → `long`) is an
// admitted conversion, so the sema check must ACCEPT it (no over-rejection).
// (The separate codegen defect that fails to *lower* this widening in the
// arrow-body path is out of scope for #1774 — this test asserts only the sema
// verdict, which the pipeline reaches before codegen.)
BEZEL_TEST(arrow_body_return_1774, int_body_long_return_widening_accepted_spec_5_1) {
    auto r = check(
        "module M; "
        "class C "
        "  public method Pub(): long => 42; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §5.1 (#1774): a genuinely function-typed return still accepts a method
// reference of the matching signature — the check rejects only a Function into
// a NON-Function target, not Function-into-Function.
BEZEL_TEST(arrow_body_return_1774, method_ref_body_function_return_accepted) {
    auto r = check(
        "module M; "
        "class C "
        "  public method Helper(): int => 1; "
        "  public method Get(): () -> int => this.Helper; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
