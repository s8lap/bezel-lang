// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_ref_out_exact_unification_1686_test.cpp
//   (BEZEL_TEST generic_ref_out_exact_unification_1686, void_swap_mismatched_types_rejected).
//
// §14.5 / #1686 primary repro: `Swap<T>(ref a: T, ref b: T)` called with a
// `ref int` and a `ref long` — the two `ref` occurrences of `T` disagree.
// Before the fix this compiled silently (inference never ran for a void
// method); it must be REJECTED with the conflicting-bindings diagnostic.

module M; static class Utils public static method Swap<T>(ref a: T, ref b: T) var temp := a; a := b; b := temp; end Swap; end Utils; class C public static method Main(): int var x: int := 1; var y: long := 2; Utils.Swap(ref x, ref y); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type parameter `T` cannot be bound to both `int` and a different type — all arguments for a type parameter must agree, and a `ref`/`out` position unifies exactly, with no conversion admitted
// CHECK-SAME: (§14.5)
