// Ported from tests/sema/primitive_assignability_test.cpp
//   (BEZEL_TEST sema_assign, int_to_int_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method M() var x: int := 42; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
