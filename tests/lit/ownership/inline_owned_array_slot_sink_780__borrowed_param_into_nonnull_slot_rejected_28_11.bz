// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/ownership/inline_owned_array_slot_sink_780_test.cpp
//   (BEZEL_TEST inline_owned_array_slot_sink_780, borrowed_param_into_nonnull_slot_rejected_28_11).
//
// §28.11 / §28.3: a non-nullable owning slot `[N owned]Cell` is equally an owned
// sink. A borrowed parameter into it is REJECTED.

module M; class Cell public n: int := 0; end Cell; class C public static method Stash(c: Cell): () var a: [2 owned]Cell := [new Cell(), new Cell()]; a[0] := c; end Stash; public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a borrow cannot be stored into an owning array slot: the array disposes each owned element when the array is disposed, but a borrow is owned elsewhere and its real owner disposes it too — a double-free
// CHECK-SAME: (§28.11 / §28.3 — store an owned value with `new`, `move`, an owned-returning call, or a consumed `owned` parameter)
