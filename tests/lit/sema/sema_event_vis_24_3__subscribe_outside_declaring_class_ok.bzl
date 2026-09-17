// Ported from tests/sema/event_visibility_test.cpp
//   (BEZEL_TEST sema_event_vis_24_3, subscribe_outside_declaring_class_ok).
//
// §24: "Subscribe is available everywhere — inside or outside the
// declaring class — because it is on the public interface." So a `Subscribe`
// call from outside is NOT flagged by the visibility fence.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Ev(N: int); class Button public event Clicked: Ev; public method Inside() _ := this; end Inside; end Button; class Other public method Outside(b: Button) var s := b.Clicked.Subscribe(lambda(e) => do end); end Outside; end Other;

// NODIAG-NOT: error
// NODIAG-NOT: §
