// Regression tests for #5279 -- §28.11 "an element borrow" / §28.3 *Borrow
// lifetime rules* (spec v0.313): a for-in iteration variable over a borrowing
// collection is an element borrow of the collection ("a default-iteration
// variable"), so returning it through an `owned` return is "a borrow cannot be
// returned as an `owned` value". Before the fix every shape below was accepted
// (the loop variable is not a tracked local, so ownedArgSourceIsBorrow fell
// through to "not a borrow") and the program leaked.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

constexpr const char* kNeedle = "a borrow cannot be returned as an `owned` value";

std::string wrap(const std::string& body) {
    return "module M; class Item public Id: int; "
           "  public constructor(i: int) Id := i; end; end Item; "
           "class C " + body + " end C;";
}

}  // namespace

// The issue's repro: collection-literal local, array element.
BEZEL_TEST(owned_return_iter_var_borrow_5279, literal_local_array_element_rejected_spec_28_11_v0_313) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Get(): owned Array<int> "
        "  var deep: Array<Array<int>> := [[7, 8, 9], [4]]; "
        "  for var row in deep do "
        "    if row.Count > 2 then return row; end; "
        "  end; "
        "  return [0, 0, 0]; "
        "end Get;"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kNeedle));
}

// Non-literal local (call result): the same hole.
BEZEL_TEST(owned_return_iter_var_borrow_5279, call_result_local_rejected_spec_28_11_v0_313) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Make(): owned Array<Array<int>> return [[1]]; end Make; "
        "public static method Get(): owned Array<int> "
        "  using var deep: Array<Array<int>> := C.Make(); "
        "  for var row in deep do return row; end; "
        "  return [0]; "
        "end Get;"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kNeedle));
}

// Borrowed collection parameter.
BEZEL_TEST(owned_return_iter_var_borrow_5279, plain_param_collection_rejected_spec_28_11_v0_313) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Get(deep: Array<Array<int>>): owned Array<int> "
        "  for var row in deep do return row; end; "
        "  return [0]; "
        "end Get;"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kNeedle));
}

// Class-typed element.
BEZEL_TEST(owned_return_iter_var_borrow_5279, class_element_rejected_spec_28_11_v0_313) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Get(): owned Item "
        "  var xs: Array<Item> := [new Item(1), new Item(2)]; "
        "  for var it in xs do return it; end; "
        "  return new Item(0); "
        "end Get;"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kNeedle));
}

// `move row` of the loop variable is no more an owned value.
BEZEL_TEST(owned_return_iter_var_borrow_5279, move_of_loop_variable_rejected_spec_28_11_v0_313) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Get(): owned Item "
        "  var xs: Array<Item> := [new Item(1)]; "
        "  for var it in xs do return move it; end; "
        "  return new Item(0); "
        "end Get;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Accept control: a string element returned through a plain return (string is
// runtime-managed, no `owned` spelling exists for it).
BEZEL_TEST(owned_return_iter_var_borrow_5279, string_element_plain_return_accepted_spec_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Get(): string "
        "  var xs: Array<string> := [\"a\", \"b\"]; "
        "  for var s in xs do return s; end; "
        "  return \"z\"; "
        "end Get;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Accept control: a value element returned by value.
BEZEL_TEST(owned_return_iter_var_borrow_5279, value_element_return_accepted_spec_28_11) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Get(): int "
        "  var xs: Array<int> := [1, 2]; "
        "  for var x in xs do return x; end; "
        "  return 0; "
        "end Get;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Accept control: an instance method returns an element borrow through a
// plain (non-owned) return; the source is the receiver's field (§28.3).
BEZEL_TEST(owned_return_iter_var_borrow_5279, instance_plain_return_of_field_element_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; class Item public Id: int; "
        "  public constructor(i: int) Id := i; end; end Item; "
        "class Box private _xs: Array<Item>; "
        "  public constructor() _xs := [new Item(1)]; end; "
        "  public method First(): Item "
        "    for var it in _xs do return it; end; "
        "    return _xs[0]; "
        "  end First; "
        "end Box;");
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, kNeedle));
}

// Accept control: a fresh value built from the loop variable is a legal owned return.
BEZEL_TEST(owned_return_iter_var_borrow_5279, fresh_value_from_loop_variable_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Get(): owned Item "
        "  var xs: Array<Item> := [new Item(1)]; "
        "  for var it in xs do return new Item(it.Id); end; "
        "  return new Item(0); "
        "end Get;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
