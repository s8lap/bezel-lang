// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/new_expr_args_test.cpp
//   (BEZEL_TEST sema_new_args, too_many_args_diagnoses).

module M; class T public constructor(a: int) end constructor; end T; class C public method M() _ := new T(1, 2); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `new T` supplies 2 argument(s) but the parameter list has 1
// CHECK-SAME: (§22.5)
