// fix #838 / §2.11 / §28.11: an OwnedArray<Subscription> (or other owning
// collection) must dispose its Subscription elements at scope-end / Clear /
// remove — a Subscription is a runtime-managed handle (bz_subscription_t*)
// disposed by bz_subscription_dispose, which unsubscribes the handler. The
// element-policy classifier fell through to the Value policy (null disposer) for
// the Subscription element type (a builtin with no declaration), so the
// subscriptions leaked: the handler kept firing after the collection died.
//
// Value tests — the program returns box.V (the running total the event handler
// accumulates). If the subscription was disposed, post-scope Fire() is a no-op.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
// Event plumbing: a Button raises Clicked(Ev); a Box accumulates the payloads.
const char* kPre =
    "module M; record Ev(N: int); "
    "class Box public V: int := 0; "
    "  public method Add(x: int) V := V + x; end Add; end Box; "
    "class Button public event Clicked: Ev; "
    "  public method Fire(n: int) Clicked.Raise(new Ev(n)); end Fire; "
    "end Button; class C ";
}  // namespace

// Scope-end disposal of the OwnedArray<Subscription> unsubscribes the handler:
// Fire(2) inside scope adds 2; Fire(5) after scope is a no-op → 2 (was 7).
BEZEL_TEST(owned_array_subscription_838, scope_end_unsubscribes) {
    BEZEL_EXPECT_EQ(run(std::string(kPre) +
        "public static method Main(): int "
        "  using var box := new Box(); using var b := new Button(); "
        "  do "
        "    using var subs := new OwnedArray<Subscription>(); "
        "    subs.Add(move b.Clicked.Subscribe((e) => do box.Add(e.N); end)); "
        "    b.Fire(2); "
        "  end; "
        "  b.Fire(5); "
        "  return box.V; "
        "end Main; end C;"), 2);
}

// Two subscriptions in the array: both unsubscribed at scope-end.
BEZEL_TEST(owned_array_subscription_838, multiple_subscriptions_unsubscribed) {
    BEZEL_EXPECT_EQ(run(std::string(kPre) +
        "public static method Main(): int "
        "  using var box := new Box(); "
        "  using var b1 := new Button(); using var b2 := new Button(); "
        "  do "
        "    using var subs := new OwnedArray<Subscription>(); "
        "    subs.Add(move b1.Clicked.Subscribe((e) => do box.Add(e.N); end)); "
        "    subs.Add(move b2.Clicked.Subscribe((e) => do box.Add(e.N); end)); "
        "    b1.Fire(1); b2.Fire(2); "
        "  end; "
        "  b1.Fire(10); b2.Fire(20); "
        "  return box.V; "                // 3
        "end Main; end C;"), 3);
}

// Clear() disposes the elements eagerly: after Clear, Fire is a no-op.
BEZEL_TEST(owned_array_subscription_838, clear_disposes_subscriptions) {
    BEZEL_EXPECT_EQ(run(std::string(kPre) +
        "public static method Main(): int "
        "  using var box := new Box(); using var b := new Button(); "
        "  using var subs := new OwnedArray<Subscription>(); "
        "  subs.Add(move b.Clicked.Subscribe((e) => do box.Add(e.N); end)); "
        "  b.Fire(4); "
        "  subs.Clear(); "
        "  b.Fire(8); "
        "  return box.V; "               // 4
        "end Main; end C;"), 4);
}
