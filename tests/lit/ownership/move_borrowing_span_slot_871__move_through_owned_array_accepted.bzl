// Ported from tests/ownership/move_borrowing_span_slot_871_test.cpp
//   (BEZEL_TEST move_borrowing_span_slot_871, move_through_owned_array_accepted).
//
// §28.11: `move owner[0]` on a CONCRETE `OwningArray<Cell>` is an enumerated
// admitting receiver — still accepted. The span gate must not touch it.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Cell public constructor() end; destructor end; end Cell; class C public static method Main(): int using var owner := new OwningArray<Cell>(); owner.Add((new Cell())); using var x := move owner[0]; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
