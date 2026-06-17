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

// D11 / §2.11 / §24.4: Subscription disposal is type-based, not tied to a
// literal `.Subscribe(...)` declarator. A Subscription obtained from a
// METHOD whose return type is `Subscription` disposes the same way: `.Dispose()`
// removes the handler, so the subsequent Raise is a no-op. (Before the fix the
// recognition was purely syntactic — a method-returned Subscription's `.Dispose`
// was a no-op and the handler stayed subscribed, returning 50 here.)
BEZEL_TEST(codegen_events_24, dispose_method_returned_subscription_unsubscribes) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Ev(N: int); "
        "class Box "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Box; "
        "class Button "
        "  public event Clicked: Ev; "
        "  public method Fire(n: int) Clicked.Raise(new Ev(n)); end Fire; "
        "  public method Wire(box: Box): Subscription "         // returns Subscription
        "    return Clicked.Subscribe((e) => do box.Add(e.N); end); "
        "  end Wire; "
        "end Button; "
        "class C public static method Main(): int "
        "  var box := new Box(); var b := new Button(); "
        "  var s := b.Wire(box); "                              // method-returned Subscription
        "  s.Dispose(); "                                       // §2.11 unsubscribe
        "  b.Fire(50); "                                        // handler gone — no-op
        "  return box.V; "
        "end Main; end C;"), 0);
}

// D11 / §2.11 / §28.3: a `using var` of type `Subscription` auto-disposes at
// scope end (the §28.3 disposal point) — removing the handler. Here the handler
// adds 5 while the subscription is live (inside the inner block), then the inner
// `using var` disposes at block end, so the outer Fire(100) does nothing. (Before
// the fix the scope-end path didn't recognize a Subscription using-var, so the
// handler survived and the result was 5 + 100 = 105.)
BEZEL_TEST(codegen_events_24, using_var_subscription_scope_end_auto_disposes) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Ev(N: int); "
        "class Box "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Box; "
        "class Button "
        "  public event Clicked: Ev; "
        "  public method Fire(n: int) Clicked.Raise(new Ev(n)); end Fire; "
        "end Button; "
        "class C public static method Main(): int "
        "  using var box := new Box(); using var b := new Button(); "
        "  do "
        "    using var s := b.Clicked.Subscribe((e) => do box.Add(e.N); end); "
        "    b.Fire(5); "                       // handler live → +5
        "  end; "                               // §28.3: s disposes here
        "  b.Fire(100); "                       // handler gone — no-op
        "  return box.V; "
        "end Main; end C;"), 5);
}

// D11 / §2.11: a Subscription passed as a PARAMETER disposes by type. The helper
// receives `s: Subscription` and calls `s.Dispose()`, which unsubscribes the
// original handler. (Before the fix a Subscription parameter lowered to a
// non-pointer and the `.Dispose()` was skipped, returning 7.)
BEZEL_TEST(codegen_events_24, dispose_parameter_subscription_unsubscribes) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Ev(N: int); "
        "class Box "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Box; "
        "class Button "
        "  public event Clicked: Ev; "
        "  public method Fire(n: int) Clicked.Raise(new Ev(n)); end Fire; "
        "end Button; "
        "class Helper "
        "  public static method Kill(s: Subscription) s.Dispose(); end Kill; "
        "end Helper; "
        "class C public static method Main(): int "
        "  var box := new Box(); var b := new Button(); "
        "  var s := b.Clicked.Subscribe((e) => do box.Add(e.N); end); "
        "  Helper.Kill(s); "                    // dispose through a parameter
        "  b.Fire(7); "                         // handler gone — no-op
        "  return box.V; "
        "end Main; end C;"), 0);
}

// D11 / §2.11: a Subscription held in a FIELD disposes by type. `Holder.sub` is
// `Subscription?`; `Stop()` disposes it, unsubscribing the handler. (Before the
// fix a Subscription field lowered to a non-pointer and the `.Dispose()` was
// skipped, returning 9.)
BEZEL_TEST(codegen_events_24, dispose_field_subscription_unsubscribes) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Ev(N: int); "
        "class Box "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Box; "
        "class Button "
        "  public event Clicked: Ev; "
        "  public method Fire(n: int) Clicked.Raise(new Ev(n)); end Fire; "
        "end Button; "
        "class Holder "
        "  public sub: Subscription?; "
        "  public method Stop() sub!.Dispose(); end Stop; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var box := new Box(); using var b := new Button(); "
        "  using var h := new Holder(); "
        "  h.sub := b.Clicked.Subscribe((e) => do box.Add(e.N); end); "
        "  h.Stop(); "                          // dispose through a field
        "  b.Fire(9); "                         // handler gone — no-op
        "  return box.V; "
        "end Main; end C;"), 0);
}

// D11 control / §2.11: the original literal-declarator form must keep working.
// `var s := evt.Subscribe(h); s.Dispose();` (the syntactic form recognized
// before the fix) still unsubscribes — Fire(5) before dispose adds 5, the later
// Fire(100) is a no-op.
BEZEL_TEST(codegen_events_24, dispose_literal_declarator_subscription_still_works) {
    BEZEL_EXPECT_EQ(run(prog(
        "var box := new Box(); var b := new Button(); "
        "var s := b.Clicked.Subscribe((e) => do box.Add(e.N); end); "
        "b.Fire(5); "                           // handler live → +5
        "s.Dispose(); "                         // literal declarator dispose
        "b.Fire(100); "                         // handler gone — no-op
        "return box.V; ")), 5);
}

// D5 / §2.11 / §15.1: a Subscription stored into a [1]Subscription inline array
// whose handler captures a value-typed (int) outer local. The [1] case (array
// size exactly 1) previously SIGSEGV'd because Subscription lowered to a
// non-pointer (i32), corrupting the single-element inline-array storage. With
// Subscription lowering to a pointer the storage is correct and the program runs.
BEZEL_TEST(codegen_events_24, subscription_in_size1_inline_array_runs_d5) {
    BEZEL_EXPECT_EQ(run(prog(
        "var total := 0; "
        "var b := new Button(); "
        "var subs: [1]Subscription; "
        "subs[0] := b.Clicked.Subscribe((e) => do total := total + e.N; end); "
        "return 42; ")), 42);
}

// D5 / §2.11 / §15.1: disposing the Subscription held in a [1]Subscription array
// element unsubscribes the handler. Fire(3) while live adds 3; after the
// element's `.Dispose()`, Fire(40) is a no-op.
BEZEL_TEST(codegen_events_24, dispose_size1_inline_array_subscription_element) {
    BEZEL_EXPECT_EQ(run(prog(
        "var box := new Box(); var b := new Button(); "
        "var subs: [1]Subscription; "
        "subs[0] := b.Clicked.Subscribe((e) => do box.Add(e.N); end); "
        "b.Fire(3); "                           // handler live → +3
        "subs[0].Dispose(); "                   // dispose the array element
        "b.Fire(40); "                          // handler gone — no-op
        "return box.V; ")), 3);
}

// #737 / §24.5: a no-payload (unit `()`) STATIC event raised via the explicit
// `Raise(())` form must dispatch its handlers — `Raise()` and `Raise(())` are
// equivalent. Previously the unit literal `()` (a non-empty arg) made codegen
// lower a UnitExpr (→ null) and bail before emitEventRaise, silently dropping
// the whole dispatch (returned 0). Now gated on the payload TYPE.
BEZEL_TEST(codegen_events, unit_static_event_raise_unit_literal_737) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class App "
        "  public static count: int := 0; "
        "  public static event Tick; "
        "  public static method Fire() Tick.Raise(()); end Fire; "
        "end App; "
        "class C public static method Main(): int "
        "  var sub := App.Tick.Subscribe((_) => do App.count := App.count + 1; end); "
        "  App.Fire(); App.Fire(); "
        "  return App.count; "  // 2
        "end Main; end C;"), 2);
}

// #737 regression: the bare `Raise()` no-arg form still dispatches (2).
BEZEL_TEST(codegen_events, unit_static_event_raise_no_arg_737) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class App "
        "  public static count: int := 0; "
        "  public static event Tick; "
        "  public static method Fire() Tick.Raise(); end Fire; "
        "end App; "
        "class C public static method Main(): int "
        "  var sub := App.Tick.Subscribe((_) => do App.count := App.count + 1; end); "
        "  App.Fire(); App.Fire(); "
        "  return App.count; "  // 2
        "end Main; end C;"), 2);
}

// §24.5 (#747 R42): a handler that throws stops the dispatch — subsequent
// handlers are NOT invoked and the exception propagates (here caught by the
// caller's try). The Raise loop now tests the global exception slot after each
// handler; pre-fix it ran every handler regardless, so `second` became 1.
BEZEL_TEST(codegen_events, throwing_handler_aborts_dispatch_747) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Ev(N: int); "
        "class Src public event Changed: Ev; "
        "  public method Fire(n: int) Changed.Raise(new Ev(n)); end Fire; end Src; "
        "class C "
        "  public static second: int := 0; "
        "  public static method Main(): int "
        "    using var s := new Src(); "
        "    using var s1 := s.Changed.Subscribe((e) => do throw new Exception(\"x\"); end); "
        "    using var s2 := s.Changed.Subscribe((e) => do second := 1; end); "
        "    try s.Fire(1); catch var ex: Exception => second := second + 0; end; "
        "    return second; "  // 0 — the second handler never ran
        "  end Main; end C;"), 0);
}

// §24.5 regression guard: with no throw, every handler runs in subscription
// order (the pending-check breaks the loop only on an actual exception).
BEZEL_TEST(codegen_events, all_handlers_run_when_no_throw_747) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Ev(N: int); "
        "class Src public event Changed: Ev; "
        "  public method Fire(n: int) Changed.Raise(new Ev(n)); end Fire; end Src; "
        "class C "
        "  public static total: int := 0; "
        "  public static method Main(): int "
        "    using var s := new Src(); "
        "    using var s1 := s.Changed.Subscribe((e) => do total := total + e.N; end); "
        "    using var s2 := s.Changed.Subscribe((e) => do total := total + 10; end); "
        "    s.Fire(5); return total; "  // 15
        "  end Main; end C;"), 15);
}
