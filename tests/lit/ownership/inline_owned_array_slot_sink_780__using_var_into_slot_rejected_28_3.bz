// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/ownership/inline_owned_array_slot_sink_780_test.cpp
//   (BEZEL_TEST inline_owned_array_slot_sink_780, using_var_into_slot_rejected_28_3).
//
// §28.3: a `using var` is scope-bound and cannot be moved out of; storing it
// into an owned slot (a move-on-read into the slot) is rejected. This is the
// original #780 repro. REJECTED (the move-out-of-`using var` diagnostic).

module M; class Cell public n: int := 0; end Cell; class C public static method Main(): int do var a: [2 owned]Cell? := [new Cell(), new Cell()]; using var b := new Cell(); a[0] := b; end; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot move out of `using var` `b`; `using var` is scope-bound
// CHECK-SAME: (§28.3)
