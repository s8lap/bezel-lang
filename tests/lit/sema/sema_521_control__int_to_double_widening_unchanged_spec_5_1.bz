// Ported from tests/sema/decimal_diagnostics_521_test.cpp
//   (BEZEL_TEST sema_521_control, int_to_double_widening_unchanged_spec_5_1).
//
// §5.1: int → double widening still works.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M(a: double, b: int): int var _ := a + b; return 0; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
