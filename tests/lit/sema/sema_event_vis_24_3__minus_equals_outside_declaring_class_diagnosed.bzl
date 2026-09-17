// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/event_visibility_test.cpp
//   (BEZEL_TEST sema_event_vis_24_3, minus_equals_outside_declaring_class_diagnosed).
//
// §24: the `-=` desugaring (→ Remove) is likewise forbidden outside.

module M; record Ev(N: int); class Button public event Clicked: Ev; public method Inside() _ := this; end Inside; end Button; class Other public method Outside(b: Button) b.Clicked -= lambda(e) => do end; end Outside; end Other;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: event `Clicked` is accessed from outside its declaring class `Button`, where its static type is the subscribe-only interface `IEvent<T>` — the `-=` form (which desugars to `Remove`) is not available; only `.Subscribe` may be used here
// CHECK-SAME: (§24)
