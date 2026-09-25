// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_inconsistent_type_arg_896_test.cpp
//   (BEZEL_TEST generic_inconsistent_type_arg_896, explicit_type_arg_with_bool_rejected).
//
// §14.1 / #896: explicit <int> with a bool? second arg is still a compile
// error (the argument type is inconsistent with the substituted T?=int?).
//
// fix #4935 (§14.5 phase 3, v0.307): before the fix the spurious
// "inconsistent type-argument inference" diagnostic fired because the
// argument-inference loop ran even with explicit type args. After the
// fix that loop is gated on `!hadExplicitTypeArgs` and the genuine
// §5.1 non-convertibility check (which runs later in typeCall) catches
// the bool?-vs-int? mismatch with the correct diagnostic.

module M; static class P public static method Merge<T>(a: T?, b: T?): T where T: value return a ?? b ?? default(T); end Merge; public static method Main(): int var x: int? := 1; var y: bool? := true; var r := Merge<int>(x, y); return 0; end Main; end P;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the argument `b` to call to `Merge` has a type that is not implicitly convertible to the target type (§5.1)
// CHECK-SAME: between integers and `bool` is never implicit
