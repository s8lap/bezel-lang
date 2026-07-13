// Ported from tests/sema/assignment_assignability_test.cpp
//   (BEZEL_TEST sema_assign2, int_to_int_local_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method M(p: int, b: bool) var x: int := 0; x := 5; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
