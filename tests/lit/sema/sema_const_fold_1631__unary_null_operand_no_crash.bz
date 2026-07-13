// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/const_fold_null_operand_1631_test.cpp
//   (BEZEL_TEST sema_const_fold_1631, unary_null_operand_no_crash).
//
// UnaryExpr with a null operand (`-;`).

module M; class C public const K: int := -; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: expected expression
