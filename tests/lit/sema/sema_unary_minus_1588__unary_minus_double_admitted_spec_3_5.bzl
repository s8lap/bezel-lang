// Ported from tests/sema/unary_minus_1588_test.cpp
//   (BEZEL_TEST sema_unary_minus_1588, unary_minus_double_admitted_spec_3_5).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method M(a: double) _ := -a; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
