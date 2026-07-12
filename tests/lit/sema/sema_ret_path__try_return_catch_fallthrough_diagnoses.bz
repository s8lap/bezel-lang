// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/definite_return_test.cpp
//   (BEZEL_TEST sema_ret_path, try_return_catch_fallthrough_diagnoses).
//
// Boundary (negative control): a catch arm that genuinely FALLS THROUGH (an
// expression statement that is NOT a throw) must still be diagnosed — the fix
// recognizes only ThrowExpr expression-statements as always-exiting.

module M; class C public method F(): int try return 1; catch ArgumentException => var z: int := 5; end; end F; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: method `F` has a return type but does not return a value on every path
// CHECK-SAME: (§21.7)
