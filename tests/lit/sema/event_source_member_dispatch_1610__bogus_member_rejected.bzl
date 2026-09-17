// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/event_source_member_dispatch_1610_test.cpp
//   (BEZEL_TEST event_source_member_dispatch_1610, bogus_member_rejected).
//
// ---------------------------------------------------------------------------
// (1) §6.5 / §24 / #1610: a BOGUS member on an `EventSource<T>` parameter is
// now rejected (before the fix it was silently accepted). The diagnostic
// names the type and the documented surface.
// ---------------------------------------------------------------------------

module Probe; import System.Threading; class Ev public N: int := 0; end Ev; public class Counter public event Ticked: Ev; public method Fire(es: EventSource<Ev>) es.TotallyBogusMethodThatDoesNotExist(42); end Fire; end Counter;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `TotallyBogusMethodThatDoesNotExist` on `EventSource<T>`; documented surface: Subscribe(handler), Raise(payload), Add(handler), Remove(handler) (stdlib §6.5 / §24)
