// Ported from tests/sema/unary_minus_1588_test.cpp
//   (BEZEL_TEST sema_unary_minus_1588, unary_minus_nullable_int_lifts_spec_7_11).
//
// §7.11: unary `-` lifts over T? to yield T? (nullable).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method M(a: int?) _ := -a; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
