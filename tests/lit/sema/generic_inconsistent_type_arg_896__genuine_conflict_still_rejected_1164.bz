// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_inconsistent_type_arg_896_test.cpp
//   (BEZEL_TEST generic_inconsistent_type_arg_896, genuine_conflict_still_rejected_1164).
//
// §22.4 / §14.1 / #1164 negative guard: a GENUINE conflict between two value
// arguments (`G<T>(a: T, b: T)` called `G(5, "x")`) is STILL correctly rejected
// — the #1164 fix only skips inline out-var args, not real value arguments.

module M; static class U public static method G<T>(a: T, b: T): T where T: value return a; end G; public static method Main(): int var r := U.G(5, "x"); return 0; end Main; end U;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: inconsistent type-argument inference: type parameter `T` cannot be bound to both `int` and a different type — all arguments for a type parameter must agree
// CHECK-SAME: (§14.1, #896)
