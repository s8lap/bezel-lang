// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_ref_out_exact_unification_1686_test.cpp
//   (BEZEL_TEST generic_ref_out_exact_unification_1686, explicit_type_arg_ref_widen_rejected).
//
// §14.5 / #1686: the ref/out-exactness half, isolated from inference
// agreement. `StoreW<T>(value: T, ref slot: T)` called with an EXPLICIT
// `<long>` type argument (so inference never runs) and a `ref` argument
// whose OWN static type is `int`, not `long` — the by-value argument
// itself agrees exactly (its type IS `long`), so only the ref argument's
// exactness is at stake. Must be REJECTED: a `long`-typed generic
// substitution cannot alias an `int` storage location.

module M; static class Utils public static method StoreW<T>(value: T, ref slot: T) slot := value; end StoreW; end Utils; class C public static method Main(): int var v: long := 5; var s: int := 1; Utils.StoreW<long>(v, ref s); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the `ref` argument for parameter `slot` does not unify exactly with the inferred type `long` — a `ref`/`out` position aliases the caller's location, so no conversion is admitted, unlike an ordinary by-value argument
// CHECK-SAME: (§14.5)
