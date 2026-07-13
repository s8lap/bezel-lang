// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/event_source_nameable_1536_test.cpp
//   (BEZEL_TEST event_source_nameable_1536, async_event_source_extra_arity_diagnosed).
//
// ---------------------------------------------------------------------------
// (5) Arity check: `AsyncEventSource` with too many generic arguments is
// likewise diagnosed.
// ---------------------------------------------------------------------------

module Probe; import System.Threading; record TickEvent(N: int); public class Counter public method Helper(es: AsyncEventSource<TickEvent, TickEvent>) end Helper; end Counter;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: generic-argument count does not match built-in type `AsyncEventSource`
