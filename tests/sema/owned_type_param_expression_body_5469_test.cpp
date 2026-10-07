// fix #5469 (§22.2 / §21.7 / §5.1 / §28.1, v0.315): a method, property
// getter, or indexer with an `owned T` return type (a bare type parameter
// marked `owned`) must reject a body that produces a concrete class type.
// `owned T` is `T` plus a transfer obligation (§28.1) — the body must still
// produce a `T`, not a different class. §5.1 lists no conversion from a
// concrete class to a bare type parameter, so `method M<T>(): owned T =>
// new Cell(7)` is rejected at the declaration.
//
// The ruling (2026-10-07):
// - "The body is checked against the declared return type." §22.2's `=>
// expression` replaces the block, which makes it the method's `return
// expression;`, and that is checked against the declared return type.
// - "`owned` does not change which type is required." `owned T` is `T` plus
// a transfer obligation (§28.1). An `owned T` return must still produce a
// `T`, exactly as the unowned `T` row that is already rejected does.
// - "§5.1 has no conversion from a concrete class to a type parameter."
// The only type-parameter conversion runs the other way: from `T` to a bound
// its constraints guarantee. `Cell` → `T` is not implicit, so the body is
// rejected under §5.1 at the declaration.
// - "A generic factory that wants 'some owned thing' declares the concrete
// return (`owned Cell<T>`) or constrains and constructs `T` (`where T: new()`
// with `new T()`)."

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

// §22.2 / §5.1 / §28.1 (#5469): `owned T` expression body returning a
// concrete class is rejected with the §5.1 diagnostic.
BEZEL_TEST(owned_type_param_expression_body_5469,
           owned_T_arrow_body_rejects_concrete_class_spec_5_1) {
    auto r = check(
        "module M; "
        "class Cell end Cell; "
        "class C "
        "  public method M<T>(): owned T => new Cell(); "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasReturnConvertError(r));
}

// §22.2 / §5.1 / §28.1 (#5469): the unowned form (`T`) was already rejected
// — sanity anchor showing the `owned` form now matches.
BEZEL_TEST(owned_type_param_expression_body_5469,
           unowned_T_arrow_body_rejects_concrete_class_spec_5_1) {
    auto r = check(
        "module M; "
        "class Cell end Cell; "
        "class C "
        "  public method M<T>(): T => new Cell(); "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasReturnConvertError(r));
}

// §22.2 / §21.7 / §5.1 / §28.1 (#5469): block-form `owned T` return with
// `return new Cell(7);` is also rejected.
BEZEL_TEST(owned_type_param_expression_body_5469,
           owned_T_block_body_rejects_concrete_class_spec_5_1) {
    auto r = check(
        "module M; "
        "class Cell end Cell; "
        "class C "
        "  public method M<T>(): owned T "
        "    return new Cell(); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasReturnConvertError(r));
}

// §28.1 (#5469): `owned Cell` returning `new Cell()` is accepted — the
// concrete owned return is the correct form for a factory that produces a Cell.
BEZEL_TEST(owned_type_param_expression_body_5469,
           owned_Cell_arrow_body_accepts_concrete_class) {
    auto r = check(
        "module M; "
        "class Cell end Cell; "
        "class C "
        "  public method M(): owned Cell => new Cell(); "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §5.1 / §14.2 (#5469): a `where T: Cell` constraint does NOT make
// `owned T => new Cell(7)` legal. The constraint runs the other way:
// `T` can be used as a `Cell`, not a `Cell` can be used as `T`.
// The body still produces `Cell`, not `T`.
BEZEL_TEST(owned_type_param_expression_body_5469,
           owned_T_with_Cell_constraint_still_rejects_concrete_class) {
    auto r = check(
        "module M; "
        "class Cell end Cell; "
        "class C "
        "  public method M<T>(): owned T where T: Cell => new Cell(); "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasReturnConvertError(r));
}
