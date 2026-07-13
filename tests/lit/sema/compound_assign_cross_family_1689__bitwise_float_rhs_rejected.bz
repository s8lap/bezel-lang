// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/compound_assign_cross_family_1689_test.cpp
//   (BEZEL_TEST compound_assign_cross_family_1689, bitwise_float_rhs_rejected).
//
// §6.5: bitwise/shift operators require BOTH operands to be true integer
// types. The pre-existing compound-assignment guard checked only the
// TARGET's kind; `x &= 1.5` on `x: int` slipped through with a bad
// right-hand operand and crashed LLVM IR verification ("Both operands to a
// binary operator are not of the same type!", `and i32 %_, double _`).

module M; class C public static method Main(): int var x: int := 5; x &= 1.5; return x; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `&=` is not valid on floating-point types — bitwise and shift operators require integer operands
// CHECK-SAME: (§6.5)
