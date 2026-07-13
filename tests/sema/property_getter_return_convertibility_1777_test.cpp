// §5.1 / §23.2 / §23.3 (issue #1777): a property GETTER body — the
// expression-body form (`property P: T => E;`, §23.2), a block `get => E;`, or
// a block `get => do … return E; … end;` (§23.3) — yields the property's value,
// exactly as a value method's body yields its return (§22.2's `=> E ≡ return
// E`). It must run the SAME §5.1 return-type convertibility check a value
// method runs, against the property's declared type as the "return type".
//
// This is the SAME gap #1774 just fixed for methods, which the method-scoped
// fix did not cover: a property decl never set returnCtx_ / currentReturnType_,
// so BOTH the arrow-body check (runMethodBody, #1774) and the block-form
// `return E;` check (the ReturnStmt "Slice T3" arm) — each gated on
// `returnCtx_ == ValueMethod && currentReturnType_` — were skipped for getter
// bodies. A type-mismatched getter (`property P: int => "x"`) was therefore
// accepted and reached codegen, which emitted malformed LLVM IR ("Function
// return type does not match operand type of return inst! ret ptr / i32"), a
// verifier failure / miscompile. The fix wires the getter bodies through the
// value-method return context (property type as currentReturnType_) so they
// share that check; a SETTER body is a void statement context and is exempt.
//
// This test pins that each getter form now rejects a type mismatch and that
// valid getters (exact, widening, get+set) still compile clean.

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

// §5.1 / §23.2 (#1777): the expression-body property form — a `string` body for
// an `int` property is rejected, exactly as the equivalent method form is.
BEZEL_TEST(property_getter_return_1777, exprbody_string_int_rejected_spec_5_1) {
    auto r = check(
        "module M; "
        "class C "
        "  public property P: int => \"x\"; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasReturnConvertError(r));
}

// §5.1 / §23.3 (#1777): a block-form getter with an expression accessor body
// (`get => "x";`) — same mismatch, same rejection.
BEZEL_TEST(property_getter_return_1777, block_expr_getter_string_int_rejected_spec_5_1) {
    auto r = check(
        "module M; "
        "class C "
        "  public property P: int "
        "    get => \"x\"; "
        "  end P; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasReturnConvertError(r));
}

// §5.1 / §23.3 (#1777): a block-form getter with a statement accessor body
// whose `return E;` mismatches (`get => do return "x"; end;`) — the ReturnStmt
// "Slice T3" arm now fires because the getter runs in a value-method context.
BEZEL_TEST(property_getter_return_1777, block_stmt_getter_return_string_int_rejected_spec_5_1) {
    auto r = check(
        "module M; "
        "class C "
        "  public property P: int "
        "    get => do return \"x\"; end; "
        "  end P; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasReturnConvertError(r));
}

// §5.1 / §23.2 / §27.6 (#1777): a paren-less method reference (`this.Helper`)
// has a Function type `() -> int`, not `int` — the same reproducer #1774 fixed
// for methods, here on the getter path.
BEZEL_TEST(property_getter_return_1777, methodref_getter_function_into_int_rejected_spec_5_1) {
    auto r = check(
        "module M; "
        "class C "
        "  public method Helper(): int => 1; "
        "  public property P: int => this.Helper; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasReturnConvertError(r));
}

// §5.1 / §23.2 (#1777): a VALID exact-type expression-body getter must still compile
// with no error — the new check must not over-reject a well-typed body.
BEZEL_TEST(property_getter_return_1777, exprbody_int_int_accepted) {
    auto r = check(
        "module M; "
        "class C "
        "  public property P: int => 42; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §5.1 / §23.2 (#1777): a §5.1 IMPLICIT WIDENING (`int` constant → `long`) is admitted,
// so the getter check must ACCEPT it — parallel to the method widening case.
BEZEL_TEST(property_getter_return_1777, exprbody_int_long_widening_accepted_spec_5_1) {
    auto r = check(
        "module M; "
        "class C "
        "  public property P: long => 42; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §5.1 / §23.3 (#1777): a valid block-form property with BOTH a getter and a
// setter compiles clean — the setter body is a void statement context (its
// `=> _x := value;` runs for effect, not as a returned value) and must not be
// swept into the getter's value-return check.
BEZEL_TEST(property_getter_return_1777, block_get_set_valid_accepted) {
    auto r = check(
        "module M; "
        "class C "
        "  private _x: int; "
        "  public property P: int "
        "    get => _x; "
        "    set => do _x := value; end; "
        "  end P; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
