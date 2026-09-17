// Ported from tests/sema/event_direct_bogus_1639_test.cpp
//   (BEZEL_TEST event_direct_bogus_1639, real_members_accepted_direct_access).
//
// ---------------------------------------------------------------------------
// (3) §6.5 / §24 / #1639: the documented producer-side surface — Raise,
// Add, Remove, Subscribe, and the compiler-generated Dispose — still
// compiles clean on a direct access from inside the declaring class.
// (Control: the fix must not over-reject real members.)
// ---------------------------------------------------------------------------
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module Probe; import System.Threading; class Ev public N: int := 0; public constructor(n: int) N := n; end constructor; end Ev; public class Counter public event Ticked: Ev; public method Fire() Ticked.Raise(new Ev(1)); Ticked.Add(lambda(e: Ev) => do end); Ticked.Remove(lambda(e: Ev) => do end); Ticked.Dispose(); end Fire; public method Sub() using var s := Ticked.Subscribe(lambda(e: Ev) => do end); end Sub; end Counter;

// NODIAG-NOT: error
// NODIAG-NOT: §
