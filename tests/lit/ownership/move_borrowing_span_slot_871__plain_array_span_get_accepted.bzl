// Ported from tests/ownership/move_borrowing_span_slot_871_test.cpp
//   (BEZEL_TEST move_borrowing_span_slot_871, plain_array_span_get_accepted).
//
// §28.11 / §4.15: a plain indexer-get on an `ArraySpan<T>` (no `move`) reads the
// borrow and is unaffected by this fix — still accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Cell public V: int := 0; end Cell; class C public static method Main(): int const arr: Array<Cell> := []; var c1 := new Cell(); arr.Add(move c1); var sp := arr.AsSpan(); var v := sp[0].V; return v; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
