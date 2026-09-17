// Ported from tests/sema/event_visibility_test.cpp
//   (BEZEL_TEST sema_event_vis_24_3, derived_raise_protected_base_event_ok).
//
// §24: "Derived classes may raise `protected` events declared on their base
// class because the protected access exposes the EventSource form." A subclass
// raising a base-class `protected` event is allowed.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Ev(N: int); class Base protected event Clicked: Ev; end Base; class Derived: Base public method Fire() Clicked.Raise(new Ev(1)); end Fire; end Derived;

// NODIAG-NOT: error
// NODIAG-NOT: §
