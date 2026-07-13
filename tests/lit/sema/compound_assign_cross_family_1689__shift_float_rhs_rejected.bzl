// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/compound_assign_cross_family_1689_test.cpp
//   (BEZEL_TEST compound_assign_cross_family_1689, shift_float_rhs_rejected).
//
// Same gap, `<<=`: `s <<= 1.5` on `s: short` crashed IR verification the
// same way ("shl i16 %_, i0 0" — the malformed count operand).

module M; class C public static method Main(): int var s: short := 21; s <<= 1.5; return int(s); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `<<=` is not valid on floating-point types — bitwise and shift operators require integer operands
// CHECK-SAME: (§6.5)
