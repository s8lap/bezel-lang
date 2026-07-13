// Ported from tests/sema/event_source_nameable_1536_test.cpp
//   (BEZEL_TEST event_source_nameable_1536, async_event_source_resolves_with_import).
//
// ---------------------------------------------------------------------------
// (2) §2.3 / §6.5 / #1536: `AsyncEventSource<T>` — the async sibling — names
// the same way.
// ---------------------------------------------------------------------------
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module Probe; import System.Threading; record TickEvent(N: int); public class Counter public async event Ticked: TickEvent; public method Helper(es: AsyncEventSource<TickEvent>) end Helper; end Counter;

// NODIAG-NOT: error
// NODIAG-NOT: §
