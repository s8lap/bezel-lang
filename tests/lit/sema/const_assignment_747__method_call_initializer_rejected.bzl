// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/const_assignment_747_test.cpp
//   (BEZEL_TEST const_assignment_747, method_call_initializer_rejected).
//
// §22.12: a const initialized by a method call is not constant — rejected.

module M; class C public static method Compute(): int return 7; end Compute; public const K: int := Compute(); public static method Main(): int return K; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: const `K` must be initialized with a compile-time-constant expression — a literal, an operation on constants, or a reference to another const or enum constant
// CHECK-SAME: (§22.12)
