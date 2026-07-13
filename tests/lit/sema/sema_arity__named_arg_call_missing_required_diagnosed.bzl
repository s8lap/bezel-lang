// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/call_arity_test.cpp
//   (BEZEL_TEST sema_arity, named_arg_call_missing_required_diagnosed).

module M; class C public method F(a: int, b: int) end F; public method M() this.F(a := 1); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: call to `F`: required parameter `b` is not supplied
// CHECK-SAME: (§22.5)
