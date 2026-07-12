// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/ownership/assignment_consumes_test.cpp
//   (BEZEL_TEST own_assign_move, read_after_move_assign_diagnoses).

module M; class T end T; class C public method M() var src := new T(); var dst: T; dst := move src; _ := src; using var hold := move dst; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: owned value in `src` was already moved out at this point
// CHECK-SAME: (§28.3)
