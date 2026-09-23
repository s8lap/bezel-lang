// Ported from tests/sema/decimal_diagnostics_521_test.cpp
//   (BEZEL_TEST sema_521_static_field, static_decimal_m_literal_init_accepted).
//
// §4.4 / §22.8: `static _v: decimal := 1.5m` → no sema error.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is
// legal), but it DOES emit a diagnostic — `_v` is a mutable static of
// primitive type, which §22.12 rule 2 form (iii) (#1393 warned-admission
// ruling, v0.221) admits only with a declaration-site warning; unrelated
// to this test's own §4.4/§22.8 subject (decimal literal suffix parsing),
// so only pinned here to keep the ACCEPT assertion accurate rather than
// silently stale.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public static _v: decimal := 1.5m; public static method Main(): int => 0; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} mutable static field `_v` has type `decimal` — a primitive/enum value static is admitted only with a declaration-site warning (§22.12 rule 2 form (iii))
// CHECK-SAME: (§22.12 rule 2, v0.221, #1393)
