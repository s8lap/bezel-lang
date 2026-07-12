// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/const_type_test.cpp
//   (BEZEL_TEST sema_const, const_mod_by_zero_rejected_1211).
//
// §22.12 (#1211): `mod 0` is equally a division by zero in a constant
// expression and must also be rejected at compile time.

module M; class T end T; class C public const Y: int := 5 mod 0; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: const `Y` initializer contains a division or modulo by zero — a const initializer must fold to a representable compile-time-constant value
// CHECK-SAME: (§22.12)
