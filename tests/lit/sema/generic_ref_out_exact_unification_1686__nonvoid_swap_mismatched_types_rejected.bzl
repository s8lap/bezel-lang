// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_ref_out_exact_unification_1686_test.cpp
//   (BEZEL_TEST generic_ref_out_exact_unification_1686, nonvoid_swap_mismatched_types_rejected).
//
// §14.5 / #1686 regression guard: the same conflict, but on a NON-void
// overload (`SwapR<T>(ref a: T, ref b: T): T`) — the pre-fix code already
// caught this shape (inference ran as a side effect of the return-type
// substitution); confirms the new unconditional check doesn't regress or
// double-diagnose it.

module M; static class Utils public static method SwapR<T>(ref a: T, ref b: T): T var temp := a; a := b; b := temp; return temp; end SwapR; end Utils; class C public static method Main(): int var x: int := 1; var y: long := 2; var r := Utils.SwapR(ref x, ref y); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type parameter `T` cannot be bound to both `int` and a different type — all arguments for a type parameter must agree, and a `ref`/`out` position unifies exactly, with no conversion admitted
// CHECK-SAME: (§14.5)
