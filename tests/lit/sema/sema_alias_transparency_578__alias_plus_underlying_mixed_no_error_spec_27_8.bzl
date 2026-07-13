// Ported from tests/sema/alias_transparency_578_test.cpp
//   (BEZEL_TEST sema_alias_transparency_578, alias_plus_underlying_mixed_no_error_spec_27_8).
//
// 6a §27.8 — Meters + double (mixed: alias + underlying type). §5.1 numeric
// widening: both sides are double, so no widening needed.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; type Meters := double; class C public static method M(a: Meters, b: double): double return a + b; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
