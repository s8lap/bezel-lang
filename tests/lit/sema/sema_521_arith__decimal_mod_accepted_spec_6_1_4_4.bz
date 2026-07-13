// Ported from tests/sema/decimal_diagnostics_521_test.cpp
//   (BEZEL_TEST sema_521_arith, decimal_mod_accepted_spec_6_1_4_4).
//
// §6.1 / §4.4: `mod` on two decimal parameters → accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M(a: decimal, b: decimal): int var _ := a mod b; return 0; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
