// Ported from tests/codegen/shared_event_source_admit_1832_test.cpp.
//
// ---------------------------------------------------------------------------
// (1) §2.3 / §6.5 / #1832: `SharedEventSource<T>` names cleanly as a
// parameter type after `import System.Threading;` — the cross-domain
// publication-point pair (§6.5 v0.187). Sibling of the #1536 instance-pair
// test; pre-#1832 the name was rejected at sema with `unresolved type name
// SharedEventSource`. The interface head `ISharedEvent<T>` follows the
// §2.13 rule 1 auto-import rule and is admitted even without an explicit
// import.
// ---------------------------------------------------------------------------
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module Probe; import System.Threading; record TickEvent(N: int); public class Bus public method Helper(s: SharedEventSource<TickEvent>) end Helper; public method Listen(s: ISharedEvent<TickEvent>) end Listen; end Bus;

// NODIAG-NOT: error
// NODIAG-NOT: §