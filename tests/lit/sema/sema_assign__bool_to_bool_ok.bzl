// Ported from tests/sema/primitive_assignability_test.cpp
//   (BEZEL_TEST sema_assign, bool_to_bool_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method M() var x: bool := true; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
