// Codegen — subscribing through a stored / method-returned IEvent<T> value
// (§24.3, #747 R42).
//
// `var ev: IEvent<Ev> := s.Happened; ev.Subscribe(h)` produced a subscription
// that NEVER FIRED (the handler was never registered). Four facets, all rooted
// in "an IEvent<T> value is the bare bz_event_source_t* (a borrow of the
// declaring instance's backing source)": (D) IEvent<T> was missing from the
// heap-pointer lowering so the local was an i32 slot; (B) an event read in value
// position (`b.Clicked` / bare `Clicked`) lowered to garbage; (C) `.Subscribe`
// on a stored IEvent<T> value was not dispatched (tryLowerEventCall only handled
// a direct event access); (A) the result was not recognized as an owned
// Subscription, so the `using var` never disposed it. The fix routes by static
// type (IEvent<T>) directly to bz_event_subscribe (no itable is fabricated).

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// A payload record `Ev`, an accumulator `Box`, and a `Bell` that declares the
// event and fires it. Common prefix for the cases below.
const char* kPrefix =
    "module M; "
    "class Ev public N: int := 0; "
    "  public constructor(n: int) N := n; end constructor; end Ev; "
    "class Box public total: int := 0; "
    "  public method Add(n: int) total := total + n; end Add; end Box; "
    "class Bell "
    "  public event Clicked: Ev; "
    "  public method GetEvent(): IEvent<Ev> return Clicked; end GetEvent; "
    "  public method Fire(n: int) Clicked.Raise(new Ev(n)); end Fire; "
    "end Bell; ";

std::string prog(const std::string& body) {
    return std::string(kPrefix) +
        "class C public static method Main(): int " + body + " end Main; end C;";
}
}  // namespace

// The headline bug: subscribe through a stored IEvent<Ev> local → handler fires.
BEZEL_TEST(codegen_event_ievent_var_747, stored_var_subscribe_fires) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var box := new Box(); using var b := new Bell(); "
        "var ev: IEvent<Ev> := b.Clicked; "
        "using var s := ev.Subscribe(lambda(e: Ev) => do box.Add(e.N); end); "
        "b.Fire(5); b.Fire(7); "
        "return box.total;")), 12);
}

// An UNTYPED handler lambda `(e) => …` through a stored IEvent<Ev> — the payload
// type is seeded from the receiver's IEvent<Ev> so the param resolves.
BEZEL_TEST(codegen_event_ievent_var_747, stored_var_untyped_lambda_fires) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var box := new Box(); using var b := new Bell(); "
        "var ev: IEvent<Ev> := b.Clicked; "
        "using var s := ev.Subscribe(lambda(e) => do box.Add(e.N); end); "
        "b.Fire(5); b.Fire(7); "
        "return box.total;")), 12);
}

// A method-RETURNED IEvent<Ev> (`return Clicked;` then `var ev := b.GetEvent()`).
// Exercises the bare-event value read + the inferred IEvent<Ev> local lowering.
BEZEL_TEST(codegen_event_ievent_var_747, method_returned_ievent_subscribe_fires) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var box := new Box(); using var b := new Bell(); "
        "var ev := b.GetEvent(); "
        "using var s := ev.Subscribe(lambda(e: Ev) => do box.Add(e.N); end); "
        "b.Fire(5); b.Fire(7); "
        "return box.total;")), 12);
}

// The stored-var Subscription is an owned Subscription: disposing it (here at the
// `do…end` scope) unsubscribes, so a later Fire does NOT reach the handler.
BEZEL_TEST(codegen_event_ievent_var_747, stored_var_subscription_disposes) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var box := new Box(); using var b := new Bell(); "
        "var ev: IEvent<Ev> := b.Clicked; "
        "do using var s := ev.Subscribe(lambda(e: Ev) => do box.Add(e.N); end); "
        "   b.Fire(5); end; "
        "b.Fire(7); "
        "return box.total;")), 5);
}

// CONTROL: a direct `b.Clicked.Subscribe(...)` must keep working.
BEZEL_TEST(codegen_event_ievent_var_747, direct_subscribe_unregressed) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var box := new Box(); using var b := new Bell(); "
        "using var s := b.Clicked.Subscribe(lambda(e: Ev) => do box.Add(e.N); end); "
        "b.Fire(5); b.Fire(7); "
        "return box.total;")), 12);
}
