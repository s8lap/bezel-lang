// Ported from tests/sema/decimal_diagnostics_521_test.cpp
//   (BEZEL_TEST sema_521_arith, decimal_add_accepted_spec_6_1).
//
// §6.1: `+` on two decimal parameters → accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M(a: decimal, b: decimal): int var _ := a + b; return 0; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
