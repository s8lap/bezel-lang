// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/try_convention_1336_test.cpp
//   (BEZEL_TEST try_convention_1336_sema, inline_out_var_infers_nullable_direct_use_rejected).
//
// (c) inline `out var x` infers `int?` — a non-nullable use without `!` REJECTS
// wherever the recipient is NOT narrowed. #5312 (stdlib §1 v0.313: shape-(a)
// members carry the `when true` postcondition, spec §22.4) narrows it INSIDE the
// controlled `then` region (accepted: ..._narrowed_in_then_accepted); the
// rejection is retained for the region after the `if`, where the call may have
// returned false.

module M; class C public static method Main(): int do const s: Stack<int> := [1]; if s.TryPop(out var x) then end; return x; end; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the returned value is nullable but the target type is non-nullable
// CHECK-SAME: (§7)
