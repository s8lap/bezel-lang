// Ported from tests/sema/decimal_diagnostics_521_test.cpp
//   (BEZEL_TEST sema_521_control, double_div_no_error_spec_6_3).
//
// §6.3: double division (not SIGFPE) still works.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M(a: double, b: double): int var _ := a / b; return 0; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
