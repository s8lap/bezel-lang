// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/const_fold_null_operand_1631_test.cpp
//   (BEZEL_TEST sema_const_fold_1631, nested_null_operand_no_crash).
//
// Nested shape: the null operand is not the immediate child of the folded
// root but reached after one level of genuine recursion (`(1 + ;) * 2`
// parenthesizes the malformed `1 + ;` sub-expression so it survives as a
// BinaryExpr operand of the outer `*`), exercising the recursive call sites
// rather than only the top-level one.

module M; class C public const K: int := (1 + ) * 2; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: expected expression
