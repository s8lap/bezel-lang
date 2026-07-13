// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/mixed_numeric_test.cpp
//   (BEZEL_TEST sema_mixed_numeric, int_plus_uint_is_error_spec_6_2).
//
// ---------------------------------------------------------------------------
// §6.2 / §5.1 — signed/unsigned pairs of the same or narrower width have no
// implicit conversion either way (§5.1: "Between signed and unsigned types
// of the same or narrower width") → §6.2 compile error.
// ---------------------------------------------------------------------------

module M; class C public static method M(a: int, b: uint): int var r := a + b; return 0; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `+` has no implicit conversion between operand types `int` and `uint`
// CHECK-SAME: (§6.2)
