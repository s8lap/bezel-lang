// Ported from tests/sema/static_event_capture_free_1542_test.cpp
//   (BEZEL_TEST static_event_capture_free_1542, plus_equals_noncapturing_lambda_over_static_field_accepted_22_12_5).
//
// §22.12 rule 5: a non-capturing lambda that touches only a STATIC field (no
// local, no parameter, no `this`) has an empty capture set and is accepted —
// "the same admission as Domain.Spawn entries" (§30.14's isCaptureFreeEntry
// treats a static-field reference identically: not a capture).
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is
// legal), but it DOES emit a diagnostic — `Total` is a mutable static of
// primitive type, which §22.12 rule 2 form (iii) (#1393 warned-admission
// ruling, v0.221) admits only with a declaration-site warning; unrelated
// to this test's own §22.12 rule 5 subject (capture-freedom of the lambda
// touching it), so only pinned here to keep the ACCEPT assertion accurate
// rather than silently stale.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; record Ev(N: int); class Bus public static Total: int := 0; public static event Signal: Ev; public static method Hook() Signal += lambda(e) => do Bus.Total := Bus.Total + e.N; end; end Hook; end Bus;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} mutable static field `Total` has type `int` — a primitive/enum value static is admitted only with a declaration-site warning (§22.12 rule 2 form (iii))
// CHECK-SAME: (§22.12 rule 2, v0.221, #1393)
