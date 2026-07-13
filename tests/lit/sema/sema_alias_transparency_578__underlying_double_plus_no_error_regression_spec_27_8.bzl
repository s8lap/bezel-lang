// Ported from tests/sema/alias_transparency_578_test.cpp
//   (BEZEL_TEST sema_alias_transparency_578, underlying_double_plus_no_error_regression_spec_27_8).
//
// Control: direct underlying-type use still works (baseline correctness).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M(a: double, b: double): double return a + b; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
