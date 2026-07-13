// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/ownership/move_borrowing_span_slot_871_test.cpp
//   (BEZEL_TEST move_borrowing_span_slot_871, move_through_readonly_array_span_rejected).
//
// §28.11 / §4.15: `ReadOnlyArraySpan<Cell>` is read-only and borrowing — `move`
// through it is rejected for the same reason.

module M; class Cell public constructor() end; destructor end; end Cell; class C public static method Main(): int using var arr := new Array<Cell>(); using var c1 := new Cell(); arr.Add(c1); var sp := arr.AsReadOnlySpan(); using var taken := move sp[0]; return 42; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `move` is not admitted through a borrowing span slot: `ReadOnlyArraySpan` views a borrowing source (a non-owning `Array<T>`, `string`, or read-only buffer), so its slots hold borrows with no ownership to transfer
// CHECK-SAME: (§28.11 / §4.15 — `move arrSpan[i]` is rejected; use the plain indexer-get to read the borrow)
