// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/try_convention_1336_test.cpp
//   (BEZEL_TEST try_convention_1336_sema, inline_out_var_infers_nullable_direct_use_rejected).
//
// (c) inline `out var x` infers `int?` — a non-nullable use without `!` REJECTS.

module M; class Cell public _v: int := 0; public constructor(v: int) _v := v; end constructor; destructor end destructor; end Cell; class C public static method Main(): int do using var s := new Stack<int>(); s.Push(1); if s.TryPop(out var x) then return x; end; end; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the returned value is nullable but the target type is non-nullable
// CHECK-SAME: (§7)
