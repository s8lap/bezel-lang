// Ported from tests/sema/out_owned_transfer_860_test.cpp
//   (BEZEL_TEST out_owned_transfer_860, borrow_trypeek_accepted).
//
// §28.3 / §28.11 / #860 (BORROW-FORM GUARDRAIL): `OwningStack.TryPeek(out item)`
// produces a BORROW of the top element (stdlib §4.11), not an owned transfer —
// dropping `item` must be ACCEPTED.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Cell public _id: int := 0; public constructor(id: int) _id := id; end constructor; destructor end destructor; end Cell; class C public static method Main(): int do using var s := new OwningStack<Cell>(); s.Push((new Cell(1))); var item: Cell? := null; var ok := s.TryPeek(out item); end; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
