// Ported from tests/sema/static_event_capture_free_1542_test.cpp
//   (BEZEL_TEST static_event_capture_free_1542, plus_equals_noncapturing_lambda_over_static_field_accepted_22_12_5).
//
// §22.12 rule 5: a non-capturing lambda that touches only a STATIC field (no
// local, no parameter, no `this`) has an empty capture set and is accepted —
// "the same admission as Domain.Spawn entries" (§30.14's isCaptureFreeEntry
// treats a static-field reference identically: not a capture).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Ev(N: int); class Bus public static Total: int := 0; public static event Signal: Ev; public static method Hook() Signal += lambda(e) => do Bus.Total := Bus.Total + e.N; end; end Hook; end Bus;

// NODIAG-NOT: error
// NODIAG-NOT: §
