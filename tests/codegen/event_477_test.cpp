// Codegen for two event miscompiles, issue #477 (language spec §24).
//
// B6 — STATIC events (§24.1): "For static events, the backing field is a
//   static owned field with program-exit disposal (§22.13)." The static
//   backing EventSource must be allocated at program start (the single static
//   root, alongside the static constructors run at Main's prologue, §22.7),
//   so a static `.Subscribe()` / in-class `+=` registers handlers that fire
//   on a later `.Raise()`. Before the fix the static backing source was never
//   allocated, so `eventSourcePtr` returned null and every Subscribe/Raise was
//   silently dropped (handlers never called).
//
// B7 — interface-typed access (§24.3 / §24.7): subscribing through a receiver
//   whose static type is an interface that declares the event must reach the
//   SAME backing EventSource the implementing class raises on. Per §24.7 the
//   compiler-generated `EventSource<T>` backing field satisfies the interface
//   contract (`EventSource<T>: IEvent<T>`). Before the fix, an interface-typed
//   receiver resolved the event to the interface's own EventSymbol — which has
//   no backing field — so the Subscribe was dropped (registered on nothing),
//   while a concrete-typed receiver worked. The handler fired on Raise only
//   for the concrete subscription, never the interface one.
//
// Scope: synchronous events only (async deferred, as in event_test.cpp).
// Observation (B7 instance events): handlers mutate a shared class instance
// captured by value (the pointer is shared, §28), read back by Main as the int
// exit code, so the asserted value proves WHICH source fired and HOW MANY times.
// Observation (B6 static events, §22.12 rule 5 v0.180, #1542): a handler
// subscribed to a `static event` must be capture-free, so the B6 handlers mutate
// a public static field on the declaring class (no local/parameter/`this`
// capture) — read back as the exit code identically. Instance-event handlers
// (B7) remain capture-admitting per the rule's own final sentence.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// =====================================================================
// B6 — static events
// =====================================================================

// §24.1 / §24.4 / §24.5: a STATIC no-payload event. Subscribe from outside the
// declaring class (IEvent<()>.Subscribe), Raise twice from inside — the handler
// runs once per Raise (n == 2). Repro shape from issue #477 B6.
// §22.12 rule 5 (v0.180, #1542): the handler must be capture-free, so it mutates
// a public static field on App (no local/parameter/`this` capture) instead of a
// captured Box instance.
BEZEL_TEST(codegen_events_477, static_event_subscribe_then_raise_twice_b6) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class App "
        "  public static Total: int := 0; "
        "  public static event Signal; "
        "  public static method Raise() Signal.Raise(); end Raise; "
        "end App; "
        "class C public static method Main(): int "
        "  var sub := App.Signal.Subscribe(lambda() => do App.Total := App.Total + 1; end); "
        "  App.Raise(); App.Raise(); "
        "  sub.Dispose(); "
        "  return App.Total; "                            // expect 2
        "end Main; end C;"), 2);
}

// §24.1 / §2.11: disposing the static-event Subscription unsubscribes — a Raise
// after Dispose is a no-op. Proves the static source is a real shared source
// (not a fresh one per access) and that Dispose reaches it.
// §22.12 rule 5 (v0.180, #1542): the handler is capture-free — it mutates a
// public static field on App, not a captured Box instance.
BEZEL_TEST(codegen_events_477, static_event_dispose_unsubscribes_b6) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class App "
        "  public static Total: int := 0; "
        "  public static event Signal; "
        "  public static method Fire() Signal.Raise(); end Fire; "
        "end App; "
        "class C public static method Main(): int "
        "  var sub := App.Signal.Subscribe(lambda() => do App.Total := App.Total + 1; end); "
        "  App.Fire(); "                                  // Total = 1
        "  sub.Dispose(); "
        "  App.Fire(); "                                  // handler gone — no-op
        "  return App.Total; "                            // expect 1
        "end Main; end C;"), 1);
}

// §24.3 / §24.4: inside the declaring class, `event += handler` is the fixed
// desugaring to Add on the EventSource<T> face. On a STATIC event the added
// handler must fire on Raise. Hook is a static method; the static backing
// source is shared with the static Raise. Per §24.5 (v0.180) a static-event
// handler must be capture-free — the handler here mutates a static field
// (globally live, not a capture), which is admitted (§28.14, #1541).
BEZEL_TEST(codegen_events_477, static_event_plus_equals_inside_class_b6) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Ev(N: int); "
        "class App "
        "  public static Total: int := 0; "
        "  public static event Signal: Ev; "
        "  public static method Hook() "
        "    Signal += lambda(e) => do Total := Total + e.N; end; "  // capture-free (static field)
        "  end Hook; "
        "  public static method Fire(n: int) Signal.Raise(new Ev(n)); end Fire; "
        "end App; "
        "class C public static method Main(): int "
        "  App.Hook(); App.Fire(7); App.Fire(3); "
        "  return App.Total; "                            // expect 10
        "end Main; end C;"), 10);
}

// §24.1 / §24.2 / §24.5: a STATIC event WITH a payload record. The handler
// reads payload fields; two raises accumulate. Proves payload threading works
// for the static-backing path too.
// §22.12 rule 5 (v0.180, #1542): the handler is capture-free — it accumulates
// into a public static field on App (reading its own payload parameter `e`),
// not a captured Box instance.
BEZEL_TEST(codegen_events_477, static_event_with_payload_b6) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Ev(N: int); "
        "class App "
        "  public static Total: int := 0; "
        "  public static event Signal: Ev; "
        "  public static method Fire(n: int) Signal.Raise(new Ev(n)); end Fire; "
        "end App; "
        "class C public static method Main(): int "
        "  var sub := App.Signal.Subscribe(lambda(e) => do App.Total := App.Total + e.N; end); "
        "  App.Fire(5); App.Fire(7); "
        "  return App.Total; "                            // expect 12
        "end Main; end C;"), 12);
}

// §24.1: a static event's subscriber list is shared across all access points —
// two separate Subscribe calls both fire on a single Raise (a static source is
// program-wide, not per-access).
// §22.12 rule 5 (v0.180, #1542): both handlers are capture-free — they
// accumulate (with distinct multipliers) into a public static field on App, not
// a captured Box instance.
BEZEL_TEST(codegen_events_477, static_event_multiple_subscribers_b6) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Ev(N: int); "
        "class App "
        "  public static Total: int := 0; "
        "  public static event Signal: Ev; "
        "  public static method Fire(n: int) Signal.Raise(new Ev(n)); end Fire; "
        "end App; "
        "class C public static method Main(): int "
        "  var s1 := App.Signal.Subscribe(lambda(e) => do App.Total := App.Total + e.N; end); "
        "  var s2 := App.Signal.Subscribe(lambda(e) => do App.Total := App.Total + e.N * 10; end); "
        "  App.Fire(2); "
        "  return App.Total; "                            // expect 22 (2 + 20)
        "end Main; end C;"), 22);
}

// =====================================================================
// B7 — interface-typed event access
// =====================================================================

// §24.3 / §24.7: subscribing through an interface-typed LOCAL must reach the
// SAME backing EventSource the implementing class raises on. Repro shape from
// issue #477 B7: concrete-ref sub fires +V, interface-local sub fires +V*10;
// a single Trigger(1) → total == 11.
BEZEL_TEST(codegen_events_477, interface_local_event_subscribe_b7) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record EvType(V: int); "
        "interface ISource "
        "  event Ready: EvType; "
        "end ISource; "
        "class Source: ISource "
        "  public event Ready: EvType; "
        "  public method Trigger(v: int) Ready.Raise(new EvType(v)); end Trigger; "
        "end Source; "
        "class Acc "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Acc; "
        "class C public static method Main(): int "
        "  var acc := new Acc(); "
        "  var src := new Source(); "
        "  using var sub1 := src.Ready.Subscribe(lambda(e) => do acc.Add(e.V); end); "
        "  var i: ISource := src; "
        "  using var sub2 := i.Ready.Subscribe(lambda(e) => do acc.Add(e.V * 10); end); "
        "  src.Trigger(1); "
        "  return acc.V; "                                // expect 11
        "end Main; end C;"), 11);
}

// §24.3 / §24.7: subscribing through an interface-typed PARAMETER must also
// reach the real backing source. The helper receives `i: ISource` and returns
// a capture-free Subscription (+V*10 into the static Tot.V); the concrete sub
// adds +V. Trigger(1) -> 11. Capture-free callbacks are always admitted
// (§28.14), so the returned form is admitted on the AOT path.
BEZEL_TEST(codegen_events_477, interface_parameter_event_subscribe_b7) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record EvType(V: int); "
        "interface ISource "
        "  event Ready: EvType; "
        "end ISource; "
        "class Source: ISource "
        "  public event Ready: EvType; "
        "  public method Trigger(v: int) Ready.Raise(new EvType(v)); end Trigger; "
        "end Source; "
        "class Tot public static V: int := 0; end Tot; "
        "class Helper "
        "  public static method Wire(i: ISource): Subscription "
        "    return i.Ready.Subscribe(lambda(e) => do Tot.V := Tot.V + e.V * 10; end); "
        "  end Wire; "
        "end Helper; "
        "class C public static method Main(): int "
        "  var src := new Source(); "
        "  var sub2 := Helper.Wire(src); "
        "  var sub1 := src.Ready.Subscribe(lambda(e) => do Tot.V := Tot.V + e.V; end); "
        "  src.Trigger(1); "
        "  return Tot.V; "                                // expect 11
        "end Main; end C;"), 11);
}

// §24.3 (regression): the concrete-typed receiver path keeps working — a
// subscribe on the concrete `Source` reference fires on Raise.
BEZEL_TEST(codegen_events_477, concrete_event_subscribe_regression_b7) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record EvType(V: int); "
        "interface ISource "
        "  event Ready: EvType; "
        "end ISource; "
        "class Source: ISource "
        "  public event Ready: EvType; "
        "  public method Trigger(v: int) Ready.Raise(new EvType(v)); end Trigger; "
        "end Source; "
        "class Acc "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Acc; "
        "class C public static method Main(): int "
        "  var acc := new Acc(); "
        "  var src := new Source(); "
        "  using var sub1 := src.Ready.Subscribe(lambda(e) => do acc.Add(e.V); end); "
        "  src.Trigger(5); "
        "  return acc.V; "                                // expect 5
        "end Main; end C;"), 5);
}

// §24.3 / §24.7 / §2.11: disposing an interface-local Subscription unsubscribes
// from the real backing source — a later Trigger fires only the surviving
// concrete sub.
BEZEL_TEST(codegen_events_477, interface_local_dispose_unsubscribes_b7) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record EvType(V: int); "
        "interface ISource "
        "  event Ready: EvType; "
        "end ISource; "
        "class Source: ISource "
        "  public event Ready: EvType; "
        "  public method Trigger(v: int) Ready.Raise(new EvType(v)); end Trigger; "
        "end Source; "
        "class Acc "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Acc; "
        "class C public static method Main(): int "
        "  var acc := new Acc(); "
        "  var src := new Source(); "
        "  var i: ISource := src; "
        "  do using var sub2 := i.Ready.Subscribe(lambda(e) => do acc.Add(e.V * 10); end); "
        "  src.Trigger(1); "                              // acc.V = 10
        "  sub2.Dispose(); "                              // unsubscribe via interface-local sub
        "  src.Trigger(1); end; "                              // handler gone — no-op
        "  return acc.V; "                                // expect 10
        "end Main; end C;"), 10);
}
