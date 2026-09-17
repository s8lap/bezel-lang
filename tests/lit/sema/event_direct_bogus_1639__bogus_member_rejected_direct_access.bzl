// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/event_direct_bogus_1639_test.cpp
//   (BEZEL_TEST event_direct_bogus_1639, bogus_member_rejected_direct_access).
//
// ---------------------------------------------------------------------------
// (1) §6.5 / §24 / #1639: a bogus method on a DIRECT event access, called
// from inside the declaring class (where the producer-side face is in
// scope), is now rejected. This is the exact repro from the issue.
// ---------------------------------------------------------------------------

module Probe; import System.Threading; class Ev public N: int := 0; end Ev; public class Counter public event Ticked: Ev; public method Fire() Ticked.Bogus(); end Fire; end Counter;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `Bogus` on event `Ticked`; documented surface: Subscribe(handler), Raise(payload)/RaiseAsync(payload), Add(handler), Remove(handler) (stdlib §6.5 / §24)
