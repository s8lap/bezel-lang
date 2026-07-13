// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_inconsistent_type_arg_896_test.cpp
//   (BEZEL_TEST generic_inconsistent_type_arg_896, explicit_type_arg_with_bool_rejected).
//
// §14.1 / #896: explicit <int> with a bool? second arg is still a compile
// error (the argument type is inconsistent with the substituted T?=int?).

module M; static class P public static method Merge<T>(a: T?, b: T?): T where T: value return a ?? b ?? default(T); end Merge; public static method Main(): int var x: int? := 1; var y: bool? := true; var r := Merge<int>(x, y); return 0; end Main; end P;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: inconsistent type-argument inference: type parameter `T` cannot be bound to both `int` and a different type — all arguments for a type parameter must agree
// CHECK-SAME: (§14.1, #896)
