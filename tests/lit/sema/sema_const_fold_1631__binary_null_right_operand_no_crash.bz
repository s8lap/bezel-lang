// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/const_fold_null_operand_1631_test.cpp
//   (BEZEL_TEST sema_const_fold_1631, binary_null_right_operand_no_crash).
//
// BinaryExpr with a null RIGHT operand (`1 + ;` — the #1622 driver-level
// reproducer, replayed here past the driver's gate).

module M; class C public const K: int := 1 + ; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: expected expression
