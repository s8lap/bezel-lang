// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/try_convention_1336_test.cpp
//   (BEZEL_TEST try_convention_1336_sema, inline_out_owned_var_direct_access_rejected).
//
// (d') inline `out owned var` — a DIRECT non-nullable access without `!` REJECTS
// wherever the recipient is NOT narrowed. #5312 (stdlib §1 v0.313: shape-(a)
// members carry the `when true` postcondition, spec §22.4) narrows the recipient
// INSIDE the controlled `then` region, so the access this test pinned in the
// `then` body is now accepted (see ..._narrowed_in_then_accepted); the rejection
// is retained for the region where the call may have returned false: the ELSE
// branch.

module M; class Cell public _v: int := 0; public constructor(v: int) _v := v; end constructor; destructor end destructor; end Cell; class C public static method Main(): int do using var s := new OwningStack<Cell>(); s.Push(move (new Cell(1))); if s.TryPop(out var item) then item.Dispose(); else var v := item._v; return v; end; end; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `._v` on a nullable value requires narrowing it to non-null first
// CHECK-SAME: (§7.4)
