// Ported from tests/sema/event_source_nameable_1536_test.cpp
//   (BEZEL_TEST event_source_nameable_1536, event_source_unit_payload_resolves).
//
// ---------------------------------------------------------------------------
// (3) §2.3 / §6.5 / #1536: the no-payload event form — `EventSource<()>`
// (stdlib §6.5: "For no-payload events ... the payload is `()`") —
// resolves too, exercising the unit-type generic argument.
// ---------------------------------------------------------------------------
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module Probe; import System.Threading; public class Counter public event Done; public method Helper(es: EventSource<()>) end Helper; end Counter;

// NODIAG-NOT: error
// NODIAG-NOT: §
