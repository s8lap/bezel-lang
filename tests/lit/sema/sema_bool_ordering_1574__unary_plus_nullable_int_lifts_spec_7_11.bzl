// Ported from tests/sema/bool_ordering_1574_test.cpp
//   (BEZEL_TEST sema_bool_ordering_1574, unary_plus_nullable_int_lifts_spec_7_11).
//
// §7.11: unary `+` lifts over T? to yield T? (nullable), same as unary `-`.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method M(a: int?) _ := +a; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
