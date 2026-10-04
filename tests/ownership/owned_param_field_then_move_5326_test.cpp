// Regression tests for issue #5326 — §28.8 / K3.1 (v0.313).
// An `owned` parameter stored into an `owned` field (`this.x := p;`) and then
// moved again (`Take(move p)`) was accepted with only a §28.2 warning
// ("move from a local `p` already disposed at this point"), and the program
// crashed (null passed to `Take` after the #5218 slot-null; before it, a
// double dispose).
//
// K3.1's consume table is explicit: the post-state for "store into an `owned`
// field" is **moved-from** — "a later read is an error until reassigned".
// `handleOwnedFieldAssignment` called `markDisposedByName` for the parameter
// case, putting it in `Disposed` state and drawing the §28.2 warning regime;
// the fix calls `markMovedByName` instead, giving the correct §28.3 K3.1
// error on the second move.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// Issue repro — MEASURED on a26-5274 = r23-c28 1f42f128 + #5274 fix.
const char* kRepro5326 = R"BZL(
module M;
class Item
  public tag: int;
  public constructor(t: int) tag := t; end constructor;
end Item;
class P
  public static method Take(owned i: Item) Console.WriteLine(i.tag.ToString()); end Take;
end P;
class H
  public owned x: Item;
  public constructor(owned p: Item) this.x := p; P.Take(move p); end constructor;
end H;
class Q
  public static method Main(): int
    using var h := new H(new Item(1));
    return 0;
  end Main;
end Q;
)BZL";

// Control: a bare owned-param stored into owned field, then the field is
// borrow-read (allowed per §28.3's "bare owned-field read is a borrow") and
// finally disposed by the destructor. The parameter `p` is in moved-from state
// after the store (K3.1); the borrow-read of `this.x` does not consume it.
const char* kControlBorrowRead = R"BZL(
module M;
class Item
  public tag: int;
  public constructor(t: int) tag := t; end constructor;
  public method TagString(): string return tag.ToString(); end TagString;
end Item;
class H
  public owned x: Item;
  public constructor(owned p: Item)
    this.x := p;                              // p -> moved-from (K3.1), x owns
    Console.WriteLine(this.x!.TagString());  // borrow read of field, not consume
  end constructor;
end H;
class Q
  public static method Main(): int
    using var h := new H(new Item(42));
    return 0;
  end Main;
end Q;
)BZL";

}  // namespace

// REJECT row: the issue repro — `this.x := p` leaves `p` in moved-from state
// per K3.1; `P.Take(move p)` is a second consume of the same value and must
// be rejected with the §28.3 K3.1 error "owned value in `p` was already
// moved out". Before the fix: accepted with §28.2 warning; ASan SEGV at runtime.
BEZEL_TEST(owned_param_field_then_move_5326,
           spec_28_8_K3_1_owned_param_stored_then_moved_again_rejected) {
    auto r = runAot(kRepro5326);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // K3.1 post-state: "a later read is an error until reassigned"
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "owned value in `p` was already moved out"));
    // The specific §28.3 diagnostic, not the §28.2 warning
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§28.3"));
    // Must NOT contain the §28.2 warning text
    BEZEL_EXPECT_TRUE(!diagnosticMessageContains(r, "§28.2"));
}

// ACCEPT control: bare owned-param stored into owned field, then borrow-read
// the field. The param is moved-from by the store; the field is borrow-read
// (not consumed) and auto-disposed by the destructor. This pattern must
// remain accepted after the fix.
BEZEL_TEST(owned_param_field_then_move_5326,
           spec_K3_1_param_store_field_borrow_read_accepted) {
    auto r = runAot(kControlBorrowRead);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("42") != std::string::npos);
}
