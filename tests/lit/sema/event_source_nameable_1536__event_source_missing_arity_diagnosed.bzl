// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/event_source_nameable_1536_test.cpp
//   (BEZEL_TEST event_source_nameable_1536, event_source_missing_arity_diagnosed).
//
// ---------------------------------------------------------------------------
// (4) Arity check: `EventSource` used with no generic argument is diagnosed
// as a wrong-arity built-in reference (same class of diagnostic as every
// other builtin generic type, e.g. Channel<T>) rather than silently
// accepted or crashing.
// ---------------------------------------------------------------------------

module Probe; import System.Threading; public class Counter public method Helper(es: EventSource) end Helper; end Counter;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: generic-argument count does not match built-in type `EventSource`
