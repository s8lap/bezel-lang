// Ported from tests/sema/out_owned_transfer_860_test.cpp
//   (BEZEL_TEST out_owned_transfer_860, trypop_moved_out_accepted).
//
// §28.3 / §28.11 / #860: discharge by moving the out-owned local into another
// owning collection (`s2.Push(move item)`) — the move of `item` IS a legal
// ownership transfer (§28.3), but the §7 nullability check (orthogonal to
// ownership) correctly fires because `item` carries the TryPop's `Cell?`
// result type while `OwningStack<T>.Push` takes a non-nullable `T`.
//
// The original test asserted `NODIAG`, which pinned the buggy pre-#4322
// pre-fix-#4324 behavior — the modifier guard that #4324 removed also masked
// the §7 check, so the program was once rejected ONLY for §7 reasons but
// silently accepted when the modifier guard skipped §7. Per
// `[[fix-must-update-existing-tests]]`, this expectation updates in the same
// fix-forward.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class Cell public _id: int := 0; public constructor(id: int) _id := id; end constructor; destructor end destructor; end Cell; class C public static method Main(): int do using var s := new OwningStack<Cell>(); using var s2 := new OwningStack<Cell>(); s.Push(move (new Cell(1))); var ok := s.TryPop(out var item); s2.Push(move item); end; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the argument to `OwningStack<T>.Push` is nullable but the target type is non-nullable
// CHECK-SAME: (§7)
