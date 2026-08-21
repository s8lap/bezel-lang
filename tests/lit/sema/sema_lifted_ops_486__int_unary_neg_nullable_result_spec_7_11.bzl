// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: unary operator `-` is not defined on a nullable value type — narrow first

// Ported from tests/sema/lifted_operators_486_sema_test.cpp
// (BEZEL_TEST ['int', 'unary']int_unary_neg_nullable_result_spec_7_11).
//
// #3298 / v0.267 / proposal 0114 (override of pre-v0.267 lift acceptance):
// the operator is now REJECTED for a nullable value type operand (§7.11).

module M; class C public method M(a: int?) _ := -a; end M; end C;
