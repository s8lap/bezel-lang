// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/event_direct_bogus_1639_test.cpp
//   (BEZEL_TEST event_direct_bogus_1639, async_bogus_member_rejected_direct_access).
//
// ---------------------------------------------------------------------------
// (2) §6.5 / §24 / #1639: the async sibling — a bogus method on a direct
// `async event` access is rejected too.
// ---------------------------------------------------------------------------

module Probe; import System.Threading; class Ev public N: int := 0; end Ev; public class Counter public async event Ticked: Ev; public method Fire() Ticked.NoSuchAsyncOp(); end Fire; end Counter;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `NoSuchAsyncOp` on event `Ticked`; documented surface: Subscribe(handler), Raise(payload)/RaiseAsync(payload), Add(handler), Remove(handler) (stdlib §6.5 / §24)
