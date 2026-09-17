// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/event_visibility_test.cpp
//   (BEZEL_TEST sema_event_vis_24_3, derived_raise_public_base_event_diagnosed).
//
// §24: the derived-class allowance is specific to `protected`. A `public`
// base event raised from a subclass is still outside its declaring class, so
// the producer-side face (Raise) is not in scope — it is diagnosed. (Public is
// the maximal visibility, so this confirms the fence keys on the declaring
// CLASS, not on whether the member is otherwise visible.)

module M; record Ev(N: int); class Base public event Clicked: Ev; end Base; class Derived: Base public method Fire() this.Clicked.Raise(new Ev(1)); end Fire; end Derived;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: event `Clicked` is accessed from outside its declaring class `Base`, where its static type is the subscribe-only interface `IEvent<T>` — `.Raise` is not available; only `.Subscribe` may be used here
// CHECK-SAME: (§24)
