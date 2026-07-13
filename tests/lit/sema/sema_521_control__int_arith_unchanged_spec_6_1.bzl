// Ported from tests/sema/decimal_diagnostics_521_test.cpp
//   (BEZEL_TEST sema_521_control, int_arith_unchanged_spec_6_1).
//
// §6.1: int + int → no error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M(a: int, b: int): int var _ := a + b; return 0; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
