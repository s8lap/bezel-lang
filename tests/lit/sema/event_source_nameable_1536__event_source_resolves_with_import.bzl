// Ported from tests/sema/event_source_nameable_1536_test.cpp
//   (BEZEL_TEST event_source_nameable_1536, event_source_resolves_with_import).
//
// ---------------------------------------------------------------------------
// (1) §2.3 / §6.5 / #1536: `EventSource<T>` names cleanly as a parameter type
// after `import System.Threading;` — the issue's own repro shape (a
// helper method on the declaring class taking the in-class producer-side
// face as a parameter, §24).
// ---------------------------------------------------------------------------
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module Probe; import System.Threading; record TickEvent(N: int); public class Counter public event Ticked: TickEvent; public method Helper(es: EventSource<TickEvent>) end Helper; end Counter;

// NODIAG-NOT: error
// NODIAG-NOT: §
