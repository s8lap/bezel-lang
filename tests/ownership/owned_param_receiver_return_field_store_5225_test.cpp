// Regression tests for issue #5225 - §28.3 (spec v0.313) borrowed-field store
// rule, clause 2: "an `owned` parameter is scope-bound ... so a borrow of it is
// rejected here exactly as a borrow of a scope-bound local is"; returns table
// source 1 (`this`): a receiver-sourced return carries the RECEIVER's source
// (K2.2 composition).
//
// `_x := value.Self()` (Self() returns `this`) over an `owned` parameter was
// admitted into a non-owned field: borrowSourceForExpr had no arm for a call
// result, so the receiver-sourced borrow carried no source and the field store
// check saw nothing. The parameter is auto-dropped at method end, so the field
// dangled (heap-use-after-free). The setter-form (`set(owned)` accessor body)
// is NOT covered: accessor bodies do not run the full ownership pass today.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

const char* kItem5225 = R"BZL(
module M;
public class Item
  public tag: int;
  public constructor(t: int) tag := t; end constructor;
  public method Self(): Item return this; end Self;
end Item;
)BZL";

const char* kMain5225 = R"BZL(
public class Program
  public static method Main(): int return 0; end Main;
end Program;
)BZL";

void expectOwnedParamFieldStoreReject5225(const std::string& body) {
    auto r = runAot(std::string(kItem5225) + body + kMain5225);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "borrow of `value` stored into a field would outlive its source"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`value` is an `owned` parameter, whose value THIS method disposes "
           "at its end"));
}

}  // namespace

// The issue repro: a receiver-sourced return over an `owned` parameter.
BEZEL_TEST(owned_param_receiver_return_field_store_5225,
           spec_28_3_clause2_issue_repro_rejected) {
    expectOwnedParamFieldStoreReject5225(R"BZL(
public class B
  private _x: Item?;
  public method SetV(owned value: Item) _x := value.Self(); end SetV;
  public method Peek(): int return _x!.tag; end Peek;
end B;
)BZL");
}

// The direct form `_x := value` (already rejected before the fix; a control
// that the call-result arm and the direct arm agree).
BEZEL_TEST(owned_param_receiver_return_field_store_5225,
           spec_28_3_clause2_direct_form_rejected) {
    expectOwnedParamFieldStoreReject5225(R"BZL(
public class B
  private _x: Item?;
  public method SetV(owned value: Item) _x := value; end SetV;
end B;
)BZL");
}

// K2.2 composition through a chain of receiver-sourced returns.
BEZEL_TEST(owned_param_receiver_return_field_store_5225,
           spec_28_3_k2_2_chained_receiver_return_rejected) {
    expectOwnedParamFieldStoreReject5225(R"BZL(
public class B
  private _x: Item?;
  public method SetV(owned value: Item) _x := value.Self().Self(); end SetV;
end B;
)BZL");
}

// Same rule for the owned scope-bound LOCAL (clause 1): the call result over
// an owned local is a borrow of that local.
BEZEL_TEST(owned_param_receiver_return_field_store_5225,
           spec_28_3_clause1_owned_local_receiver_return_rejected) {
    auto r = runAot(std::string(kItem5225) + R"BZL(
public class B
  private _x: Item?;
  public method SetV() using var l := new Item(1); _x := l.Self(); end SetV;
end B;
)BZL" + kMain5225);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "borrow of `l` stored into a field would outlive its source"));
}

// Accept control (clause 2): a NON-owned parameter's receiver-sourced return
// is admitted - the caller's source outlives the call - and the field read
// after the call sees the live value. Also `this.Self()` (receiver-sourced).
BEZEL_TEST(owned_param_receiver_return_field_store_5225,
           spec_28_3_clause2_non_owned_param_and_this_accepted) {
    auto r = runAot(std::string(kItem5225) + R"BZL(
public class B
  private _x: Item?;
  private _y: B?;
  public method SetV(value: Item) _x := value.Self(); end SetV;
  public method SelfB(): B return this; end SelfB;
  public method SetThis() _y := this.SelfB(); end SetThis;
  public method Peek(): int return _x!.tag; end Peek;
  public method PeekThis(): int return _y!.Peek(); end PeekThis;
end B;
public class Program
  public static method Main(): int
    using var it := new Item(7);
    using var b := new B();
    b.SetV(it);
    b.SetThis();
    Console.WriteLine(b.Peek().ToString());
    Console.WriteLine(b.PeekThis().ToString());
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n7\n");
}
