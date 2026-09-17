// Ported from tests/sema/event_visibility_test.cpp
//   (BEZEL_TEST sema_event_vis_24_3, minus_equals_inside_declaring_class_ok).
//
// §24: `Clicked -= handler` inside the declaring class desugars to
// EventSource<T>.Remove and is allowed.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Ev(N: int); class Button public event Clicked: Ev; public method Inside() Clicked -= lambda(e) => do end; end Inside; end Button; class Other public method Outside(b: Button) _ := b; end Outside; end Other;

// NODIAG-NOT: error
// NODIAG-NOT: §
