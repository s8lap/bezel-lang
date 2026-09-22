// Ported from tests/ownership/assignment_consumes_test.cpp
//   (BEZEL_TEST own_assign_move, field_target_move_consumes_source).
//
// Reject control: bezelc -c must reject and emit a §28.3 K3.13 redundant-move diagnostic.
//
// RUN: %not %bezelc -c -o %t.o %s
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class T end T; class C private owned _t: T := new T(); public method Replace() var fresh := new T(); this._t := move fresh; end Replace; end C;


// CHECK: the `move` marker is redundant on a {{.*}} (§28.3 K3.13 marker economy; K3.1)