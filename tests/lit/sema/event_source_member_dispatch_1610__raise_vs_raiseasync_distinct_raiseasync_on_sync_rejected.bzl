// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/event_source_member_dispatch_1610_test.cpp
//   (BEZEL_TEST event_source_member_dispatch_1610, raise_vs_raiseasync_distinct)
//   — SECOND HALF (`rb`). See the sibling `..._raise_on_async_rejected.bzl`
//   for the `ra` half and the shared extraction note.
//
// ---------------------------------------------------------------------------
// (4b) §6.5 / #1610: the producer surfaces are DISTINCT — `Raise` lives on
// `EventSource<T>` only and `RaiseAsync` on `AsyncEventSource<T>` only
// (stdlib §6.5). `RaiseAsync` on an `EventSource<T>` is rejected.
// ---------------------------------------------------------------------------

module Probe; import System.Threading; class Ev public N: int := 0; public constructor(n: int) N := n; end constructor; end Ev; public class Counter public event Ticked: Ev; public method Fire(es: EventSource<Ev>) es.RaiseAsync(new Ev(1)); end Fire; end Counter;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `RaiseAsync` on `EventSource<T>`; documented surface: Subscribe(handler), Raise(payload), Add(handler), Remove(handler) (stdlib §6.5 / §24)
