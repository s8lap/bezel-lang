// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §22.3 "ref and out argument locations" + §14.5 (spec v0.228) / #2266 — the
// WIDENING direction of the same hole, which is the memory-unsafe one.
//
// `ref slot: long` on the generic method `StoreW<T>` with an `int`-typed caller
// location: §5.1 admits int -> long for an ordinary by-value argument, but a
// ref position aliases the caller's 4-byte slot, so the callee's `slot := 1`
// is an 8-byte store into it — a stack overwrite past the argument. §14.5
// already rejects this exact shape when the parameter is generic
// (generic_ref_out_exact_unification_1686__explicit_type_arg_ref_widen_rejected);
// the concretely-typed parameter on a generic method was reached by neither
// check.
//
// WITHOUT the fix this program compiles clean (exit 0), so `%not` fails.

module M; static class U public static method StoreW<T>(v: T, ref slot: long) slot := 1; end StoreW; end U; class C public static method Main(): int var i: int := 0; U.StoreW<int>(5, ref i); return i; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the `ref` argument for parameter `slot` to call to `StoreW` does not match the parameter's referent type
// CHECK-SAME: aliases the caller's location (§22.3)
// CHECK-SAME: no implicit conversion is admitted (§14.5)
