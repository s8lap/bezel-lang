// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/call_arity_test.cpp
//   (BEZEL_TEST sema_arity, too_many_args_diagnoses).

module M; class C public method F(a: int, b: int) end F; public method M() this.F(1, 2, 3); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: call to `F` supplies 3 argument(s) but the parameter list has 2
// CHECK-SAME: (§22.5)
