// Ported from tests/sema/lifted_operators_486_sema_test.cpp
//   (BEZEL_TEST sema_lifted_ops_486, int_mixed_nonnull_plus_nullable_result_spec_7_11).
//
// §7.11: int + int? → result is int?.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method M(a: int, b: int?) _ := a + b; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
