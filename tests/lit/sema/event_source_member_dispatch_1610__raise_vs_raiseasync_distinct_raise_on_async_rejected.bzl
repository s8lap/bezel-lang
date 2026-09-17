// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/event_source_member_dispatch_1610_test.cpp
//   (BEZEL_TEST event_source_member_dispatch_1610, raise_vs_raiseasync_distinct)
//   — FIRST HALF (`ra`). The original BEZEL_TEST body asserts two independent
//   diagnostics (`ra`/`rb`) via two separate parseSemaAll() calls; the
//   extractor anchors one CHECK per BEZEL_TEST case, so this is split across
//   two `.bzl` files to preserve full coverage. See the sibling
//   `..._raiseasync_on_sync_rejected.bzl` for the `rb` half.
//
// ---------------------------------------------------------------------------
// (4a) §6.5 / #1610: the producer surfaces are DISTINCT — `Raise` lives on
// `EventSource<T>` only and `RaiseAsync` on `AsyncEventSource<T>` only
// (stdlib §6.5). `Raise` on an `AsyncEventSource<T>` is rejected.
// ---------------------------------------------------------------------------

module Probe; import System.Threading; class Ev public N: int := 0; public constructor(n: int) N := n; end constructor; end Ev; public class Counter public async event Ticked: Ev; public method Fire(es: AsyncEventSource<Ev>) es.Raise(new Ev(1)); end Fire; end Counter;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `Raise` on `AsyncEventSource<T>`; documented surface: Subscribe(handler), RaiseAsync(payload), Add(handler), Remove(handler) (stdlib §6.5 / §24)
