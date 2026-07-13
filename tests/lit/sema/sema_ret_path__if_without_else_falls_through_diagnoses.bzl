// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/definite_return_test.cpp
//   (BEZEL_TEST sema_ret_path, if_without_else_falls_through_diagnoses).

module M; class C public method F(b: bool): int if b then return 1; end; end F; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: method `F` has a return type but does not return a value on every path
// CHECK-SAME: (§21.7)
