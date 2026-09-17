// Ported from tests/sema/event_direct_bogus_1639_test.cpp
//   (BEZEL_TEST event_direct_bogus_1639, subscribe_from_outside_still_accepted).
//
// ---------------------------------------------------------------------------
// (4) §24 / #1639: `Subscribe` on a direct event access from OUTSIDE the
// declaring class remains accepted (the subscribe-only `IEvent<T>` face
// is in scope everywhere) — the new bogus-member check must not
// interfere with the pre-existing §24 visibility posture.
// ---------------------------------------------------------------------------
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module Probe; import System.Threading; class Ev public N: int := 0; end Ev; public class Counter public event Ticked: Ev; end Counter; public class Outside public method Watch(c: Counter) using var sub := c.Ticked.Subscribe(lambda(e: Ev) => do end); end Watch; end Outside;

// NODIAG-NOT: error
// NODIAG-NOT: §
