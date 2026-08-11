// Ported from tests/sema/decimal_diagnostics_521_test.cpp
//   (BEZEL_TEST sema_521_arith, decimal_plus_int_param_accepted_spec_5_1_6_1).
//
// §5.1 / §6.1: int widens to decimal (§5.1), so `decimal + int` is accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M(a: decimal, b: int): int _ := a + b; return 0; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
