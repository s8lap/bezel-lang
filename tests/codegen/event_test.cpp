// Codegen for synchronous events (language spec §24). An `event`
// declaration gets a compiler-generated owned backing field of type
// EventSource<T> (§24.1). Outside the declaring class the member is seen
// as IEvent<T> and exposes only Subscribe (§24.3); inside, the class raises
// it. Subscribing returns an owned Subscription (§24.4 / stdlib §2.11);
// disposing the Subscription removes the handler. Raising invokes every
// subscribed handler in order with the payload (§24.5).
//
// Handlers are function values (closures, §27). Side effects are observed
// through a class instance the handler captures by value — the captured
// reference (a pointer) lets the handler mutate shared state, which Main then
// reads back as the int exit code.
//
// Scope: synchronous events only. Async events (IAsyncEvent<T> /
// AsyncEventSource<T>, §24.5) are deferred — async is not implemented.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

// A reusable observation type plus an event-bearing class. `Box` is mutated
// by handlers (captured by value — the pointer is shared, §28 value capture);
// `Button` declares a synchronous event with an int payload record and raises
// it from inside the class (§24.5).
const char* kPrelude =
    "module M; "
    "record Ev(N: int); "
    "class Box "
    "  public V: int := 0; "
    "  public method Add(x: int) V := V + x; end Add; "
    "end Box; "
    "class Button "
    "  public event Clicked: Ev; "                          // §24.1
    "  public method Fire(n: int) Clicked.Raise(new Ev(n)); end Fire; "  // §24.5
    "end Button; ";

std::string prog(const std::string& mainBody) {
    return std::string(kPrelude) +
           "class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

} // namespace

// §24.1 / §24.4 / §24.5: declare an event, Subscribe a lambda handler from
// outside the declaring class (IEvent<T>.Subscribe), then Raise — the handler
// runs and observes the payload.
BEZEL_TEST(codegen_events_24, subscribe_lambda_then_raise_runs_handler) {
    BEZEL_EXPECT_EQ(run(prog(
        "var box := new Box(); "
        "var b := new Button(); "
        "var s := b.Clicked.Subscribe((e) => do box.Add(e.N); end); "
        "b.Fire(5); b.Fire(7); "
        "return box.V; ")), 12);  // 5 + 7
}

// §24.5: with zero subscribers, Raise is a no-op (no null check, no error).
BEZEL_TEST(codegen_events_24, raise_with_no_subscribers_is_noop) {
    BEZEL_EXPECT_EQ(run(prog(
        "var b := new Button(); "
        "b.Fire(42); "          // no subscribers — nothing happens
        "return 7; ")), 7);
}

// §24.5: handlers run in subscription order and ALL fire on a single Raise.
BEZEL_TEST(codegen_events_24, multiple_handlers_all_fire) {
    BEZEL_EXPECT_EQ(run(prog(
        "var box := new Box(); "
        "var b := new Button(); "
        "var s1 := b.Clicked.Subscribe((e) => do box.Add(e.N); end); "
        "var s2 := b.Clicked.Subscribe((e) => do box.Add(e.N * 10); end); "
        "b.Fire(2); "
        "return box.V; ")), 22);  // 2 + 20
}

// §24.4 / §2.11: Subscribe returns an owned Subscription; disposing it removes
// the handler so subsequent raises do not invoke it.
BEZEL_TEST(codegen_events_24, dispose_subscription_unsubscribes_handler) {
    BEZEL_EXPECT_EQ(run(prog(
        "var box := new Box(); "
        "var b := new Button(); "
        "var s := b.Clicked.Subscribe((e) => do box.Add(e.N); end); "
        "b.Fire(5); "          // box.V = 5
        "s.Dispose(); "        // remove the handler (§2.11)
        "b.Fire(100); "        // no handler left — no-op
        "return box.V; ")), 5);
}

// §24.3 / §24.4: inside the declaring class, `event += handler` is the fixed
// desugaring to Add (the EventSource<T> face). The added handler fires on Raise.
BEZEL_TEST(codegen_events_24, plus_equals_adds_handler_inside_class) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Ev(N: int); "
        "class Box "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Box; "
        "class Button "
        "  public event Clicked: Ev; "
        "  public method Hook(target: Box) "
        "    Clicked += (e) => target.Add(e.N); "   // §24.4 += desugars to Add
        "  end Hook; "
        "  public method Fire(n: int) Clicked.Raise(new Ev(n)); end Fire; "
        "end Button; "
        "class C public static method Main(): int "
        "  var box := new Box(); var b := new Button(); "
        "  b.Hook(box); b.Fire(7); b.Fire(3); "
        "  return box.V; "
        "end Main; end C;"), 10);  // 7 + 3
}

// §24.1 / §24.3: a no-payload event (`event Ping;`) has payload () and a
// no-argument Raise()/() -> () handler (stdlib §6.5 EventSource<()> convenience).
BEZEL_TEST(codegen_events_24, no_payload_event_raise_and_subscribe) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box "
        "  public V: int := 0; "
        "  public method Bump() V := V + 1; end Bump; "
        "end Box; "
        "class App "
        "  public event Ping; "                       // §24.1 no payload → ()
        "  public method Fire() Ping.Raise(); end Fire; "  // §24.5 Raise()
        "end App; "
        "class C public static method Main(): int "
        "  var box := new Box(); var a := new App(); "
        "  var s := a.Ping.Subscribe(() => do box.Bump(); end); "
        "  a.Fire(); a.Fire(); a.Fire(); "
        "  return box.V; "
        "end Main; end C;"), 3);
}

// §24.5: a handler's payload-record fields are readable inside the handler.
// (The lambda parameter `e` is typed as the event payload T per §24.3.)
BEZEL_TEST(codegen_events_24, handler_reads_payload_record_fields) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Point(X: int, Y: int); "
        "class Box "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Box; "
        "class Emitter "
        "  public event Moved: Point; "
        "  public method Fire(a: int, b: int) Moved.Raise(new Point(a, b)); end Fire; "
        "end Emitter; "
        "class C public static method Main(): int "
        "  var box := new Box(); var em := new Emitter(); "
        "  var s := em.Moved.Subscribe((e) => do box.Add(e.X + e.Y); end); "
        "  em.Fire(3, 4); "
        "  return box.V; "
        "end Main; end C;"), 7);  // 3 + 4
}

// §24.1 / §28.5: an event's backing field is per-instance — two separate
// objects have independent subscriber lists.
BEZEL_TEST(codegen_events_24, event_backing_field_is_per_instance) {
    BEZEL_EXPECT_EQ(run(prog(
        "var box := new Box(); "
        "var b1 := new Button(); "
        "var b2 := new Button(); "
        "var s := b1.Clicked.Subscribe((e) => do box.Add(e.N); end); "
        "b2.Fire(99); "      // b2 has no subscribers — does not touch box
        "b1.Fire(4); "       // b1's handler fires
        "return box.V; ")), 4);
}

// §24.4 / §24.5: subscribing MORE handlers than the event's initial backing
// capacity (4) forces the handler array to grow via realloc (bz_event_add).
// All handlers must remain live and fire on Raise after the growth. This
// exercises the realloc-growth path whose return value is now captured (a
// failed realloc returns 0 without overwriting the live array, rather than
// NULLing it and dereferencing on the next write). Six handlers each add the
// payload once → 6 * N on a single Fire.
BEZEL_TEST(codegen_events_24, subscribe_past_capacity_grows_all_fire_24_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "var box := new Box(); "
        "var b := new Button(); "
        "var s1 := b.Clicked.Subscribe((e) => do box.Add(e.N); end); "
        "var s2 := b.Clicked.Subscribe((e) => do box.Add(e.N); end); "
        "var s3 := b.Clicked.Subscribe((e) => do box.Add(e.N); end); "
        "var s4 := b.Clicked.Subscribe((e) => do box.Add(e.N); end); "
        "var s5 := b.Clicked.Subscribe((e) => do box.Add(e.N); end); "  // grows past 4
        "var s6 := b.Clicked.Subscribe((e) => do box.Add(e.N); end); "
        "b.Fire(5); "
        "return box.V; ")), 30);  // 6 handlers * 5
}
