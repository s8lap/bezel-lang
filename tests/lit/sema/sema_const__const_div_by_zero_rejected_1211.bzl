// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/const_type_test.cpp
//   (BEZEL_TEST sema_const, const_div_by_zero_rejected_1211).
//
// §22.12 (#1211): a const initializer whose constant expression contains a
// division by zero must be rejected at compile time — `10 / 0` has no
// representable compile-time value.

module M; class T end T; class C public const X: int := 10 / 0; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: const `X` initializer contains a division or modulo by zero — a const initializer must fold to a representable compile-time-constant value
// CHECK-SAME: (§22.12)
