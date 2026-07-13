// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/const_fold_null_operand_1631_test.cpp
//   (BEZEL_TEST sema_const_fold_1631, binary_null_left_operand_no_crash).
//
// BinaryExpr with a null LEFT operand (`* 1` — the leading `*` is not a
// valid expression start; parseUnaryExpression returns null without
// consuming it, and the binary-operator loop then reads that unconsumed `*`
// as the operator, producing `BinaryExpr{op:"*", left:null, right:1}`).

module M; class C public const K: int := * 1; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: expected expression
