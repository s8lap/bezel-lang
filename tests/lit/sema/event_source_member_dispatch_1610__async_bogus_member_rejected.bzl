// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/event_source_member_dispatch_1610_test.cpp
//   (BEZEL_TEST event_source_member_dispatch_1610, async_bogus_member_rejected).
//
// ---------------------------------------------------------------------------
// (3) §6.5 / §24 / #1610: the async sibling — a bogus member on an
// `AsyncEventSource<T>` parameter is rejected.
// ---------------------------------------------------------------------------

module Probe; import System.Threading; class Ev public N: int := 0; end Ev; public class Counter public async event Ticked: Ev; public method Fire(es: AsyncEventSource<Ev>) es.NoSuchAsyncOp(); end Fire; end Counter;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `NoSuchAsyncOp` on `AsyncEventSource<T>`; documented surface: Subscribe(handler), RaiseAsync(payload), Add(handler), Remove(handler) (stdlib §6.5 / §24)
