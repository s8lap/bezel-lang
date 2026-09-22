// Ported from tests/sema/out_owned_transfer_860_test.cpp
//   (BEZEL_TEST out_owned_transfer_860, trypop_returned_accepted).
//
// §28.3 / §28.11 / #860: discharge by returning the out-owned local from a
// method whose return type is `owned Cell?`. Must be ACCEPTED.
//
// Reject control: bezelc -c must reject and emit a §28.3 K3.13 redundant-move diagnostic.
//
// RUN: %not %bezelc -c -o %t.o %s
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class Cell public _id: int := 0; public constructor(id: int) _id := id; end constructor; destructor end destructor; end Cell; class C public static method Take(): owned Cell? using var s := new OwningStack<Cell>(); s.Push(move (new Cell(1))); var ok := s.TryPop(out owned var item); return move item; end Take; public static method Main(): int using var c := Take(); return 0; end Main; end C;


// CHECK: the `move` marker is redundant on a {{.*}} (§28.3 K3.13 marker economy; K3.2)