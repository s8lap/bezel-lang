// Regression tests for #5267 -- §22.3 (owned parameters, `move` at the call
// site), §28.3 (v0.313). The ownership pass looked up a binding's state by
// NAME in an unordered map, so a local that shadows an `owned` parameter was
// confused with the parameter: a missed §22.3 "this call consumes `value`"
// diagnostic (the shadowing `var value := 5` found the parameter's state only
// by accident of the fallback) and a false "owned value in `value` was already
// moved out" (the inner local's `move` marked the OUTER parameter moved-from).

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

constexpr const char* kConsumes = "this call consumes `value`";
constexpr const char* kMoved = "was already moved out";

std::string prog(const std::string& members) {
    return "module M; "
           "class Item public Id: int; "
           "  public constructor(i: int) Id := i; end; end Item; "
           "class Helper "
           "  public static method Take(owned x: Item): int return x.Id; end Take; "
           "end Helper; "
           "class B private owned _o: Item?; " + members + " end B;";
}

}  // namespace

// ---- missed §22.3 diagnostic ------------------------------------------------

BEZEL_TEST(shadow_owned_param_by_symbol_5267, value_local_shadow_then_outer_take_unmarked_rejected_spec_22_3) {
    auto r = parseAndCheckOwnership(prog(
        "public method SetV(owned value: Item): int "
        "  var acc := 0; "
        "  do var value := 5; acc := acc + value; end; "
        "  acc := acc + Helper.Take(value); "
        "  return acc; "
        "end SetV;"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kConsumes));
}

BEZEL_TEST(shadow_owned_param_by_symbol_5267, setter_value_shadow_then_outer_take_unmarked_rejected_spec_23_3) {
    auto r = parseAndCheckOwnership(prog(
        "public property Val: Item "
        "  get => _o!; "
        "  set(owned) => do "
        "    var acc := 0; "
        "    do var value := 5; acc := acc + value; end; "
        "    acc := acc + Helper.Take(value); "
        "  end; "
        "end Val;"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kConsumes));
}

BEZEL_TEST(shadow_owned_param_by_symbol_5267, nested_blocks_shadow_then_outer_take_unmarked_rejected_spec_22_3) {
    auto r = parseAndCheckOwnership(prog(
        "public method SetV(owned value: Item): int "
        "  var acc := 0; "
        "  do do var value := 5; acc := acc + value; end; end; "
        "  acc := acc + Helper.Take(value); "
        "  return acc; "
        "end SetV;"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kConsumes));
}

BEZEL_TEST(shadow_owned_param_by_symbol_5267, loop_body_shadow_then_outer_take_unmarked_rejected_spec_22_3) {
    auto r = parseAndCheckOwnership(prog(
        "public method SetV(owned value: Item): int "
        "  var acc := 0; "
        "  for var i in 0..<3 do var value := 5; acc := acc + value; end; "
        "  acc := acc + Helper.Take(value); "
        "  return acc; "
        "end SetV;"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kConsumes));
}

BEZEL_TEST(shadow_owned_param_by_symbol_5267, lambda_param_shadow_then_outer_take_unmarked_rejected_spec_22_3) {
    auto r = parseAndCheckOwnership(prog(
        "public method SetV(owned value: Item): int "
        "  var f := lambda(value: int) => value + 1; "
        "  var acc := f(1); "
        "  acc := acc + Helper.Take(value); "
        "  return acc; "
        "end SetV;"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kConsumes));
}

// ---- false "already moved out" ---------------------------------------------

BEZEL_TEST(shadow_owned_param_by_symbol_5267, inner_local_moved_then_outer_store_accepted_spec_22_3) {
    auto r = parseAndCheckOwnership(prog(
        "public method SetV(owned value: Item) "
        "  do var value := new Item(9); Helper.Take(move value); end; "
        "  _o := value; "
        "end SetV;"));
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, kMoved));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(shadow_owned_param_by_symbol_5267, setter_inner_local_moved_then_outer_store_accepted_spec_23_3) {
    auto r = parseAndCheckOwnership(prog(
        "public property Val: Item "
        "  get => _o!; "
        "  set(owned) => do "
        "    do var value := new Item(9); Helper.Take(move value); end; "
        "    _o := value; "
        "  end; "
        "end Val;"));
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, kMoved));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(shadow_owned_param_by_symbol_5267, nested_blocks_inner_moved_then_outer_store_accepted_spec_22_3) {
    auto r = parseAndCheckOwnership(prog(
        "public method SetV(owned value: Item) "
        "  do do var value := new Item(9); Helper.Take(move value); end; end; "
        "  _o := value; "
        "end SetV;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(shadow_owned_param_by_symbol_5267, loop_body_inner_moved_then_outer_store_accepted_spec_22_3) {
    auto r = parseAndCheckOwnership(prog(
        "public method SetV(owned value: Item) "
        "  for var i in 0..<3 do "
        "    var value := new Item(9); Helper.Take(move value); "
        "  end; "
        "  _o := value; "
        "end SetV;"));
    // A separate pass (definite assignment) still reports a read of an
    // "unassigned local `value`" for a loop-body shadow; this row pins only
    // the ownership pass's symptom (the false "already moved out").
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, kMoved));
}

BEZEL_TEST(shadow_owned_param_by_symbol_5267, inner_local_disposed_then_outer_store_accepted_spec_28_2) {
    auto r = parseAndCheckOwnership(prog(
        "public method SetV(owned value: Item) "
        "  do var value := new Item(9); value.Dispose(); end; "
        "  _o := value; "
        "end SetV;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---- non-shadowing controls -------------------------------------------------

BEZEL_TEST(shadow_owned_param_by_symbol_5267, no_shadow_take_unmarked_rejected_control_spec_22_3) {
    auto r = parseAndCheckOwnership(prog(
        "public method SetV(owned value: Item): int "
        "  return Helper.Take(value); "
        "end SetV;"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kConsumes));
}

BEZEL_TEST(shadow_owned_param_by_symbol_5267, no_shadow_take_marked_accepted_control_spec_22_3) {
    auto r = parseAndCheckOwnership(prog(
        "public method SetV(owned value: Item): int "
        "  return Helper.Take(move value); "
        "end SetV;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(shadow_owned_param_by_symbol_5267, no_shadow_double_move_rejected_control_spec_28_3) {
    auto r = parseAndCheckOwnership(prog(
        "public method SetV(owned value: Item): int "
        "  Helper.Take(move value); "
        "  _o := value; "
        "  return 0; "
        "end SetV;"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMoved));
}

BEZEL_TEST(shadow_owned_param_by_symbol_5267, shadow_moved_inner_then_inner_reuse_rejected_control_spec_28_3) {
    auto r = parseAndCheckOwnership(prog(
        "public method SetV(owned value: Item): int "
        "  do "
        "    var value := new Item(9); "
        "    Helper.Take(move value); "
        "    Helper.Take(move value); "
        "  end; "
        "  _o := value; "
        "  return 0; "
        "end SetV;"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMoved));
}
