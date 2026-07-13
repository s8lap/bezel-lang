// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_inconsistent_type_arg_896_test.cpp
//   (BEZEL_TEST generic_inconsistent_type_arg_896, inconsistent_int_bool_rejected).
//
// §14.1 / #896: Merge<T>(int?, bool?) — T cannot be both int and bool.
// This is the primary repro; bezelc crashed (SIGABRT) before the fix.

module M; static class P public static method Merge<T>(a: T?, b: T?): T where T: value return a ?? b ?? default(T); end Merge; public static method Main(): int var x: int? := 1; var y: bool? := true; var r := Merge(x, y); return 0; end Main; end P;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: inconsistent type-argument inference: type parameter `T` cannot be bound to both `int` and a different type — all arguments for a type parameter must agree
// CHECK-SAME: (§14.1, #896)
