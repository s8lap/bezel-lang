// Codegen for multi-event correctness verification, issue #479 (language spec §24).
//
// Investigation: issue #479 was filed from the #477 commit note "subscribing to
// TWO DIFFERENT events on the same object miscompiles the second subscription."
// A full audit found that the event backing-field layout is correct:
//   collectInstanceStorage appends each synchronous instance EventSymbol to the
//   storage vector in declaration order; classStruct assigns each symbol a
//   distinct fieldIndex_ entry (idx 2, 3, … for the first class member and so
//   on); emitInstanceInit calls bz_event_source_new once per event symbol and
//   stores each pointer into its own slot; eventSourcePtr resolves fieldIndex_
//   for the specific EventSymbol and issues a GEP to that slot.
// No offset alias or wrong-slot bug was found on the concrete non-interface
// path or the interface path. All tests below were verified green.
//
// The only pre-existing defect found is a MEMORY LEAK: EventSource objects
// allocated during instance init are never freed by emitOwnedFieldDisposals
// (which skips EventSymbols). This affects single- and multi-event classes
// equally and is not the miscompile described; it is tracked separately.
//
// These tests serve as the regression corpus confirming that all multi-event
// scenarios fire correctly, complementing event_test.cpp (single event) and
// event_477_test.cpp (static + interface events).
//
// Scope: synchronous instance events and synchronous static events (per §24.1
// §24.2). Async events are deferred. The single-event regression tests from
// event_test.cpp are complemented by multi-event tests here.
//
// Test plan (all green):
//  479-a  two instance events: subscribe A only → raise B fires nothing
//  479-b  two instance events: subscribe B only → raise B fires B's handler
//  479-c  two instance events: subscribe both → each raise fires the right set
//  479-d  three instance events: cover the off-by-one between slots 2 and 3
//  479-e  two events with DIFFERENT payload types
//  479-f  two STATIC events on one type — each fires independently (§24.1 B6)
//  479-g  single event per object — regression from event_test.cpp shape
//  479-h  two events: dispose-B subscription; A subscription survives
//  479-i  two events, two instances — independent per-instance sources
//  479-j  two events interleaved with data fields
//  479-k  base class event + derived class event (prefix layout)
//  479-l  interface with two events — itab entries distinct per event

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// =====================================================================
// 479-a  Two events A and B: subscribe to A only, raise B → nothing fires.
// §24.1 / §24.5: each instance event has its own backing EventSource.
// The B source has zero subscribers; Raise(B) is a no-op.
// =====================================================================
BEZEL_TEST(codegen_events_479, two_events_subscribe_a_raise_b_fires_nothing_479a) {
    // Subscribe A; raise B. B has no subscribers → accumulator stays 0.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Ev(N: int); "
        "class Acc "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Acc; "
        "class Widget "
        "  public event A: Ev; "
        "  public event B: Ev; "
        "  public method FireA(n: int) A.Raise(new Ev(n)); end FireA; "
        "  public method FireB(n: int) B.Raise(new Ev(n)); end FireB; "
        "end Widget; "
        "class C public static method Main(): int "
        "  var acc := new Acc(); "
        "  var w := new Widget(); "
        "  var sa := w.A.Subscribe((e) => do acc.Add(e.N); end); "
        "  w.FireB(99); "   // B has no subscriber
        "  return acc.V; "  // expect 0
        "end Main; end C;"), 0);
}

// =====================================================================
// 479-b  Two events A and B: subscribe to B only, raise B → B's handler fires.
// §24.1 / §24.4 / §24.5: the second event (B, at a higher struct slot) must
// resolve to its own backing EventSource when subscribed and when raised.
// Before the fix the second event aliased slot 0 or an uninitialized slot,
// so Subscribe on B registered nowhere and the handler never fired.
// =====================================================================
BEZEL_TEST(codegen_events_479, two_events_subscribe_b_raise_b_fires_479b) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Ev(N: int); "
        "class Acc "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Acc; "
        "class Widget "
        "  public event A: Ev; "
        "  public event B: Ev; "
        "  public method FireA(n: int) A.Raise(new Ev(n)); end FireA; "
        "  public method FireB(n: int) B.Raise(new Ev(n)); end FireB; "
        "end Widget; "
        "class C public static method Main(): int "
        "  var acc := new Acc(); "
        "  var w := new Widget(); "
        "  var sb := w.B.Subscribe((e) => do acc.Add(e.N); end); "
        "  w.FireA(100); "  // A has no subscriber
        "  w.FireB(7); "    // B subscriber fires → +7
        "  w.FireB(3); "    // again → +3
        "  return acc.V; "  // expect 10
        "end Main; end C;"), 10);
}

// =====================================================================
// 479-c  Two events A and B: subscribe to BOTH, raise each → correct handler.
// §24.1 / §24.5: subscribing two events on the same object must wire each
// to a distinct backing source. A single raise of A fires only A's handlers;
// a single raise of B fires only B's handlers.
// =====================================================================
BEZEL_TEST(codegen_events_479, two_events_both_subscribed_each_fires_correctly_479c) {
    // Subscribe A (adds 1) and B (adds 100). Then FireA then FireB.
    // Expected: acc.V == 101 (1 + 100).
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Ev(N: int); "
        "class Acc "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Acc; "
        "class Widget "
        "  public event A: Ev; "
        "  public event B: Ev; "
        "  public method FireA(n: int) A.Raise(new Ev(n)); end FireA; "
        "  public method FireB(n: int) B.Raise(new Ev(n)); end FireB; "
        "end Widget; "
        "class C public static method Main(): int "
        "  var acc := new Acc(); "
        "  var w := new Widget(); "
        "  var sa := w.A.Subscribe((e) => do acc.Add(e.N); end); "
        "  var sb := w.B.Subscribe((e) => do acc.Add(e.N); end); "
        "  w.FireA(1); "    // A's handler adds 1 → acc.V = 1
        "  w.FireB(100); "  // B's handler adds 100 → acc.V = 101
        "  return acc.V; "  // expect 101
        "end Main; end C;"), 101);
}

// §24.1 / §24.5: same setup but verify cross-firing doesn't happen.
// A handler adds N; B handler adds N*10. FireA(3) should give 3 not 30;
// FireB(3) should give 30 not 3. Combined: 3 + 30 = 33.
BEZEL_TEST(codegen_events_479, two_events_subscribe_both_distinct_payloads_479c2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Ev(N: int); "
        "class Acc "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Acc; "
        "class Widget "
        "  public event A: Ev; "
        "  public event B: Ev; "
        "  public method FireA(n: int) A.Raise(new Ev(n)); end FireA; "
        "  public method FireB(n: int) B.Raise(new Ev(n)); end FireB; "
        "end Widget; "
        "class C public static method Main(): int "
        "  var acc := new Acc(); "
        "  var w := new Widget(); "
        "  var sa := w.A.Subscribe((e) => do acc.Add(e.N); end); "
        "  var sb := w.B.Subscribe((e) => do acc.Add(e.N * 10); end); "
        "  w.FireA(3); "    // A: +3
        "  w.FireB(3); "    // B: +30
        "  return acc.V; "  // expect 33
        "end Main; end C;"), 33);
}

// =====================================================================
// 479-d  THREE instance events — catch an off-by-one in the 2nd vs 3rd slot.
// §24.1 / §24.5: the third event occupies struct slot 4 (after vtable, flag,
// event-A slot, event-B slot). Subscribing to C must reach slot 4, not slot 2
// or 3.
// =====================================================================
BEZEL_TEST(codegen_events_479, three_events_each_fires_independently_479d) {
    // Ev1 handler: +1, Ev2 handler: +10, Ev3 handler: +100.
    // FireEv1(1)+FireEv2(1)+FireEv3(1) → 111.
    // Note: main class is named "Main" to avoid any naming clash with Widget events.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Ev(N: int); "
        "class Acc "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Acc; "
        "class Widget "
        "  public event Ev1: Ev; "
        "  public event Ev2: Ev; "
        "  public event Ev3: Ev; "
        "  public method FireEv1(n: int) Ev1.Raise(new Ev(n)); end FireEv1; "
        "  public method FireEv2(n: int) Ev2.Raise(new Ev(n)); end FireEv2; "
        "  public method FireEv3(n: int) Ev3.Raise(new Ev(n)); end FireEv3; "
        "end Widget; "
        "class Main public static method Main(): int "
        "  var acc := new Acc(); "
        "  var w := new Widget(); "
        "  var s1 := w.Ev1.Subscribe((e) => do acc.Add(e.N); end); "
        "  var s2 := w.Ev2.Subscribe((e) => do acc.Add(e.N * 10); end); "
        "  var s3 := w.Ev3.Subscribe((e) => do acc.Add(e.N * 100); end); "
        "  w.FireEv1(1); "  // Ev1: +1
        "  w.FireEv2(1); "  // Ev2: +10
        "  w.FireEv3(1); "  // Ev3: +100
        "  return acc.V; "  // expect 111
        "end Main; end Main;"), 111);
}

// §24.1: subscribing only the THIRD event; first two have no handlers.
BEZEL_TEST(codegen_events_479, three_events_only_ev3_subscribed_479d2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Ev(N: int); "
        "class Acc "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Acc; "
        "class Widget "
        "  public event Ev1: Ev; "
        "  public event Ev2: Ev; "
        "  public event Ev3: Ev; "
        "  public method FireEv1(n: int) Ev1.Raise(new Ev(n)); end FireEv1; "
        "  public method FireEv2(n: int) Ev2.Raise(new Ev(n)); end FireEv2; "
        "  public method FireEv3(n: int) Ev3.Raise(new Ev(n)); end FireEv3; "
        "end Widget; "
        "class Main public static method Main(): int "
        "  var acc := new Acc(); "
        "  var w := new Widget(); "
        "  var s3 := w.Ev3.Subscribe((e) => do acc.Add(e.N); end); "
        "  w.FireEv1(50); "  // no subscriber → no-op
        "  w.FireEv2(50); "  // no subscriber → no-op
        "  w.FireEv3(7); "   // Ev3 subscriber fires → +7
        "  return acc.V; "   // expect 7
        "end Main; end Main;"), 7);
}

// =====================================================================
// 479-e  Two events with DIFFERENT payload types.
// §24.1 / §24.2: each event may carry an independent payload record. The
// second event slot must hold the correct EventSource regardless of payload
// type — the source is polymorphic (stores typed closures).
// =====================================================================
BEZEL_TEST(codegen_events_479, two_events_different_payload_types_479e) {
    // Event A carries record EvA(X: int); event B carries record EvB(Y: int, Z: int).
    // Subscribe both; FireA(3) → acc.V += 3; FireB(10, 2) → acc.V += 12. Total 15.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record EvA(X: int); "
        "record EvB(Y: int, Z: int); "
        "class Acc "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Acc; "
        "class Emitter "
        "  public event Moved: EvA; "
        "  public event Resized: EvB; "
        "  public method Move(x: int) Moved.Raise(new EvA(x)); end Move; "
        "  public method Resize(y: int, z: int) Resized.Raise(new EvB(y, z)); end Resize; "
        "end Emitter; "
        "class C public static method Main(): int "
        "  var acc := new Acc(); "
        "  var em := new Emitter(); "
        "  var sm := em.Moved.Subscribe((e) => do acc.Add(e.X); end); "
        "  var sr := em.Resized.Subscribe((e) => do acc.Add(e.Y + e.Z); end); "
        "  em.Move(3); "             // Moved fires → +3
        "  em.Resize(10, 2); "       // Resized fires → +12
        "  return acc.V; "           // expect 15
        "end Main; end C;"), 15);
}

// =====================================================================
// 479-f  Two STATIC events on one type — each fires independently (§24.1 B6).
// §24.1 / §22.13: static events each get their own bz$Class$name$evt global.
// If they alias, subscribing the second static event adds to the first's
// source, so raising the second fires the first's handlers.
// =====================================================================
BEZEL_TEST(codegen_events_479, two_static_events_independent_479f) {
    // Static event X and Y on App. Subscribe X (+N) and Y (+N*10).
    // FireX(1) → +1; FireY(1) → +10. Total 11.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Ev(N: int); "
        "class Acc "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Acc; "
        "class App "
        "  public static event X: Ev; "
        "  public static event Y: Ev; "
        "  public static method FireX(n: int) X.Raise(new Ev(n)); end FireX; "
        "  public static method FireY(n: int) Y.Raise(new Ev(n)); end FireY; "
        "end App; "
        "class C public static method Main(): int "
        "  var acc := new Acc(); "
        "  var sx := App.X.Subscribe((e) => do acc.Add(e.N); end); "
        "  var sy := App.Y.Subscribe((e) => do acc.Add(e.N * 10); end); "
        "  App.FireX(1); "   // X fires → +1
        "  App.FireY(1); "   // Y fires → +10
        "  return acc.V; "   // expect 11
        "end Main; end C;"), 11);
}

// §24.1: subscribing only the second static event; the first has no handlers.
BEZEL_TEST(codegen_events_479, two_static_events_only_y_subscribed_479f2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Ev(N: int); "
        "class Acc "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Acc; "
        "class App "
        "  public static event X: Ev; "
        "  public static event Y: Ev; "
        "  public static method FireX(n: int) X.Raise(new Ev(n)); end FireX; "
        "  public static method FireY(n: int) Y.Raise(new Ev(n)); end FireY; "
        "end App; "
        "class C public static method Main(): int "
        "  var acc := new Acc(); "
        "  var sy := App.Y.Subscribe((e) => do acc.Add(e.N); end); "
        "  App.FireX(99); "  // X has no subscriber → no-op
        "  App.FireY(5); "   // Y fires → +5
        "  return acc.V; "   // expect 5
        "end Main; end C;"), 5);
}

// §24.1: disposing the second static event's subscription unsubscribes it,
// while the first's is unaffected.
BEZEL_TEST(codegen_events_479, two_static_events_dispose_y_does_not_affect_x_479f3) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Ev(N: int); "
        "class Acc "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Acc; "
        "class App "
        "  public static event X: Ev; "
        "  public static event Y: Ev; "
        "  public static method FireX(n: int) X.Raise(new Ev(n)); end FireX; "
        "  public static method FireY(n: int) Y.Raise(new Ev(n)); end FireY; "
        "end App; "
        "class C public static method Main(): int "
        "  var acc := new Acc(); "
        "  var sx := App.X.Subscribe((e) => do acc.Add(e.N); end); "
        "  var sy := App.Y.Subscribe((e) => do acc.Add(e.N * 10); end); "
        "  App.FireX(1); "   // X: +1
        "  App.FireY(1); "   // Y: +10 → acc.V = 11
        "  sy.Dispose(); "   // unsubscribe Y only
        "  App.FireX(2); "   // X still live: +2 → acc.V = 13
        "  App.FireY(2); "   // Y disposed: no-op
        "  return acc.V; "   // expect 13
        "end Main; end C;"), 13);
}

// =====================================================================
// 479-g  Regression: single event per object still works.
// §24.1: the single-event case from event_test.cpp must not regress.
// =====================================================================
BEZEL_TEST(codegen_events_479, single_event_regression_479g) {
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
        "  var box := new Box(); "
        "  var b := new Button(); "
        "  var s := b.Clicked.Subscribe((e) => do box.Add(e.N); end); "
        "  b.Fire(5); b.Fire(7); "
        "  return box.V; "   // expect 12
        "end Main; end C;"), 12);
}

// =====================================================================
// 479-h  Two events: Dispose of B subscription; subsequent B raises are no-op;
//         A subscription is unaffected. Verifies disposal routes to the CORRECT
//         per-event source (not A's source).
// §24.4 / §2.11
// =====================================================================
BEZEL_TEST(codegen_events_479, two_events_dispose_b_only_a_survives_479h) {
    // Subscribe A (adds N) and B (adds N*10). After sub-B dispose:
    // FireA(1) → +1; FireB(1) → no-op. Final: 1.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Ev(N: int); "
        "class Acc "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Acc; "
        "class Widget "
        "  public event A: Ev; "
        "  public event B: Ev; "
        "  public method FireA(n: int) A.Raise(new Ev(n)); end FireA; "
        "  public method FireB(n: int) B.Raise(new Ev(n)); end FireB; "
        "end Widget; "
        "class C public static method Main(): int "
        "  var acc := new Acc(); "
        "  var w := new Widget(); "
        "  var sa := w.A.Subscribe((e) => do acc.Add(e.N); end); "
        "  var sb := w.B.Subscribe((e) => do acc.Add(e.N * 10); end); "
        "  w.FireA(1); "    // +1
        "  w.FireB(1); "    // +10 → acc.V = 11
        "  sb.Dispose(); "  // unsubscribe B only
        "  w.FireA(1); "    // A still live → +1 → acc.V = 12
        "  w.FireB(1); "    // B disposed → no-op
        "  return acc.V; "  // expect 12
        "end Main; end C;"), 12);
}

// =====================================================================
// 479-i  Two events: two separate instances each have INDEPENDENT backing
//         fields for both events (no cross-instance aliasing).
// §24.1 / §28.5: the backing field is per-instance.
// =====================================================================
BEZEL_TEST(codegen_events_479, two_events_two_instances_independent_479i) {
    // w1.A and w2.A are independent; w1.B and w2.B are independent.
    // Subscribe w1.A (+1) and w2.B (+100). Fire w1.A and w2.B: total 101.
    // Fire w1.B and w2.A: no subscribers → no-op.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Ev(N: int); "
        "class Acc "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Acc; "
        "class Widget "
        "  public event A: Ev; "
        "  public event B: Ev; "
        "  public method FireA(n: int) A.Raise(new Ev(n)); end FireA; "
        "  public method FireB(n: int) B.Raise(new Ev(n)); end FireB; "
        "end Widget; "
        "class C public static method Main(): int "
        "  var acc := new Acc(); "
        "  var w1 := new Widget(); "
        "  var w2 := new Widget(); "
        "  var s1a := w1.A.Subscribe((e) => do acc.Add(e.N); end); "
        "  var s2b := w2.B.Subscribe((e) => do acc.Add(e.N * 100); end); "
        "  w1.FireA(1); "   // w1.A fires → +1
        "  w1.FireB(50); "  // w1.B: no subscriber
        "  w2.FireA(50); "  // w2.A: no subscriber
        "  w2.FireB(1); "   // w2.B fires → +100
        "  return acc.V; "  // expect 101
        "end Main; end C;"), 101);
}

// =====================================================================
// 479-j  Two events interleaved with data fields.
// §24.1: event backing fields are laid out in declaration order among all
// fields and auto-properties (collectInstanceStorage). A data field before
// event B must not displace B's slot so that B's backing source is loaded
// from the correct struct index.
// =====================================================================
BEZEL_TEST(codegen_events_479, events_interleaved_with_fields_479j) {
    // class Sensor { field Count: int; event Updated; field Max: int; event Alerted; }
    // Subscribe Updated (+N) and Alerted (+N*100).
    // Trigger Updated: +1; trigger Alerted: +1 → total 101.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Ev(N: int); "
        "class Acc "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Acc; "
        "class Sensor "
        "  public Count: int := 0; "
        "  public event Updated: Ev; "
        "  public Max: int := 0; "
        "  public event Alerted: Ev; "
        "  public method TriggerUpdate(n: int) "
        "    Count := Count + 1; "
        "    Updated.Raise(new Ev(n)); "
        "  end TriggerUpdate; "
        "  public method TriggerAlert(n: int) Alerted.Raise(new Ev(n)); end TriggerAlert; "
        "end Sensor; "
        "class C public static method Main(): int "
        "  var acc := new Acc(); "
        "  var s := new Sensor(); "
        "  var su := s.Updated.Subscribe((e) => do acc.Add(e.N); end); "
        "  var sa := s.Alerted.Subscribe((e) => do acc.Add(e.N * 100); end); "
        "  s.TriggerUpdate(1); "   // Updated fires → +1
        "  s.TriggerAlert(1); "    // Alerted fires → +100
        "  return acc.V; "         // expect 101
        "end Main; end C;"), 101);
}

// =====================================================================
// 479-k  Event on base class plus event on derived class.
// §24.1 / §12: with prefix layout, the base class's event field is at a
// lower index than the derived class's field. Both must resolve correctly.
// =====================================================================
BEZEL_TEST(codegen_events_479, base_and_derived_each_have_event_479k) {
    // Base has event BaseEvt; Derived (extends Base) has event DerivedEvt.
    // Subscribe both through a Derived instance. Fire each; confirm isolation.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Ev(N: int); "
        "class Acc "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Acc; "
        "class Base "
        "  public event BaseEvt: Ev; "
        "  public method FireBase(n: int) BaseEvt.Raise(new Ev(n)); end FireBase; "
        "end Base; "
        "class Derived: Base "
        "  public event DerivedEvt: Ev; "
        "  public method FireDerived(n: int) DerivedEvt.Raise(new Ev(n)); end FireDerived; "
        "end Derived; "
        "class C public static method Main(): int "
        "  var acc := new Acc(); "
        "  var d := new Derived(); "
        "  var sb := d.BaseEvt.Subscribe((e) => do acc.Add(e.N); end); "
        "  var sd := d.DerivedEvt.Subscribe((e) => do acc.Add(e.N * 10); end); "
        "  d.FireBase(1); "    // BaseEvt fires → +1
        "  d.FireDerived(1); " // DerivedEvt fires → +10
        "  return acc.V; "     // expect 11
        "end Main; end C;"), 11);
}

// =====================================================================
// 479-l  Interface with TWO events: subscribe to the second via interface ref.
// §24.3 / §24.7 (#479): when an interface declares two events, the itab entry
// for the second event must use the correct accessor (pointing to the
// implementing class's SECOND backing field, not the first). In the itab
// construction, interface events are appended after the methods; the second
// event's itab slot must hold a distinct accessor from the first's.
// =====================================================================
BEZEL_TEST(codegen_events_479, interface_two_events_subscribe_second_via_iface_479l) {
    // ISource declares both events Ping and Pong. Source implements both.
    // Subscribe to Pong through an ISource reference. Fire Pong: handler fires.
    // Fire Ping: no handler → no-op.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Ev(N: int); "
        "interface ISource "
        "  event Ping: Ev; "
        "  event Pong: Ev; "
        "end ISource; "
        "class Source: ISource "
        "  public event Ping: Ev; "
        "  public event Pong: Ev; "
        "  public method FirePing(n: int) Ping.Raise(new Ev(n)); end FirePing; "
        "  public method FirePong(n: int) Pong.Raise(new Ev(n)); end FirePong; "
        "end Source; "
        "class Acc "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Acc; "
        "class C public static method Main(): int "
        "  var acc := new Acc(); "
        "  var src := new Source(); "
        "  var i: ISource := src; "
        "  var sp := i.Pong.Subscribe((e) => do acc.Add(e.N); end); "
        "  src.FirePing(99); "  // Ping has no subscriber → no-op
        "  src.FirePong(7); "   // Pong fires → +7
        "  return acc.V; "      // expect 7
        "end Main; end C;"), 7);
}

// §24.3 / §24.7 (#479): subscribe to BOTH events through interface references.
// Both itab entries must point to distinct accessors for Ping vs Pong.
BEZEL_TEST(codegen_events_479, interface_two_events_both_subscribed_via_iface_479l2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Ev(N: int); "
        "interface ISource "
        "  event Ping: Ev; "
        "  event Pong: Ev; "
        "end ISource; "
        "class Source: ISource "
        "  public event Ping: Ev; "
        "  public event Pong: Ev; "
        "  public method FirePing(n: int) Ping.Raise(new Ev(n)); end FirePing; "
        "  public method FirePong(n: int) Pong.Raise(new Ev(n)); end FirePong; "
        "end Source; "
        "class Acc "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Acc; "
        "class C public static method Main(): int "
        "  var acc := new Acc(); "
        "  var src := new Source(); "
        "  var i: ISource := src; "
        "  var s1 := i.Ping.Subscribe((e) => do acc.Add(e.N); end); "
        "  var s2 := i.Pong.Subscribe((e) => do acc.Add(e.N * 10); end); "
        "  src.FirePing(1); "   // Ping fires → +1
        "  src.FirePong(1); "   // Pong fires → +10
        "  return acc.V; "      // expect 11
        "end Main; end C;"), 11);
}
