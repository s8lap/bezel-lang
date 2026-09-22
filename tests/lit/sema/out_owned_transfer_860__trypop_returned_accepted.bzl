// Ported from tests/sema/out_owned_transfer_860_test.cpp
//   (BEZEL_TEST out_owned_transfer_860, trypop_returned_accepted).
//
// §28.3 / §28.11 / #860: discharge by returning the out-owned local from a
// method whose return type is `owned Cell?`. Must be ACCEPTED.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Cell public _id: int := 0; public constructor(id: int) _id := id; end constructor; destructor end destructor; end Cell; class C public static method Take(): owned Cell? using var s := new OwningStack<Cell>(); s.Push(move (new Cell(1))); var ok := s.TryPop(out owned var item); return item; end Take; public static method Main(): int using var c := Take(); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
