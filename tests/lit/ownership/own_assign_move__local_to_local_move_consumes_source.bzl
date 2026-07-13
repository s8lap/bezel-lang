// Ported from tests/ownership/assignment_consumes_test.cpp
//   (BEZEL_TEST own_assign_move, local_to_local_move_consumes_source).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class T end T; class C public method M() var src := new T(); var dst: T; dst := move src; using var hold := move dst; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
