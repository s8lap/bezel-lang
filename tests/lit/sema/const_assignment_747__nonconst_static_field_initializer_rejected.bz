// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/const_assignment_747_test.cpp
//   (BEZEL_TEST const_assignment_747, nonconst_static_field_initializer_rejected).
//
// §22.12 (#747 R42): a const initialized from a (non-const) static field is
// NOT a compile-time constant — rejected. Pre-fix it was silently accepted and
// treated like a static-field initializer.

module M; class C public static _runtime: int := 5; public const K: int := _runtime * 2; public static method Main(): int return K; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: const `K` must be initialized with a compile-time-constant expression — a literal, an operation on constants, or a reference to another const or enum constant
// CHECK-SAME: (§22.12)
