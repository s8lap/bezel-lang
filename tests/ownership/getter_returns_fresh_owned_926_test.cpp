// Spec-derived tests for #926 / §28.6 (v0.153): a property/indexer GETTER may
// not return a freshly-produced owned value. A getter is a borrow-producing
// position — properties and indexers do not carry `owned` — so a fresh owned
// value returned through it escapes with no owner and leaks (the #926 repro:
// `using var c := f.Item;` where `Item`'s getter is `get => new Cell();`).
//
// §28.6 (v0.153): "A property or indexer getter may not return a freshly-produced
// owned value … Expose the factory as a method returning `owned T` instead."
//
// Covers the expression-bodied read-only property (`property X: T => E`), the
// block-form `get => E` and `get => do … end` accessors, and the owned-returning-
// call and (rejected) producers. A bare field read (a borrow) stays ACCEPTED, as
// does a METHOD returning `owned T` — the recommended factory form.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

std::string mod(const std::string& member) {
    return std::string("module M; "
        "class Cell end Cell; "
        "class Factory "
        "  private _cell: Cell?; "
        "  public static method MakeCell(): owned Cell "
        "    return new Cell(); "
        "  end MakeCell; "
        ) + member + " end Factory;";
}

} // namespace

// ---- NEGATIVE: a getter returning a fresh owned value → REJECT ----

// The #926 repro: a block-form `get => new Cell();`.
BEZEL_TEST(getter_returns_fresh_owned_926, block_getter_new_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "public property Item: Cell "
        "  get => new Cell(); "
        "end Item;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Expression-bodied read-only property `property Item: Cell => new Cell();`.
BEZEL_TEST(getter_returns_fresh_owned_926, expr_property_new_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "public property Item: Cell => new Cell();"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// A getter returning an owned-returning CALL result is also a fresh owned value.
BEZEL_TEST(getter_returns_fresh_owned_926, block_getter_owned_call_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "public property Item: Cell "
        "  get => MakeCell(); "
        "end Item;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// A block-body getter that `return`s a fresh owned value.
BEZEL_TEST(getter_returns_fresh_owned_926, block_body_getter_return_new_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "public property Item: Cell "
        "  get => do return new Cell(); end; "
        "end Item;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---- POSITIVE: must stay ACCEPTED ----

// A getter returning a bare field read is a BORROW (the legal getter form). The
// property type is nullable to mirror the field. ACCEPT.
BEZEL_TEST(getter_returns_fresh_owned_926, getter_bare_field_read_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "public property Item: Cell? "
        "  get => _cell; "
        "end Item;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The recommended alternative the diagnostic points to: a METHOD returning
// `owned T` may return a fresh `new Cell()` (methods can carry `owned`). ACCEPT.
BEZEL_TEST(getter_returns_fresh_owned_926, method_returning_owned_new_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "public method MakeItem(): owned Cell "
        "  return new Cell(); "
        "end MakeItem;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// An auto-property has no getter body to check. ACCEPT.
BEZEL_TEST(getter_returns_fresh_owned_926, auto_property_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "public property Auto: Cell?;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
