// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/event_direct_bogus_1639_test.cpp
//   (BEZEL_TEST event_direct_bogus_1639, bogus_member_rejected_from_outside).
//
// ---------------------------------------------------------------------------
// (5) §24 / #1639: a bogus member from OUTSIDE the declaring class is
// rejected by the new check (not silently accepted just because the
// visibility-fence check at that call site is gated on a fixed list of
// recognized names and does not itself fire for an unrecognized one).
// ---------------------------------------------------------------------------

module Probe; import System.Threading; class Ev public N: int := 0; end Ev; public class Counter public event Ticked: Ev; end Counter; public class Outside public method Poke(c: Counter) c.Ticked.TotallyBogus(); end Poke; end Outside;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `TotallyBogus` on event `Ticked`; documented surface: Subscribe(handler), Raise(payload)/RaiseAsync(payload), Add(handler), Remove(handler) (stdlib §6.5 / §24)
