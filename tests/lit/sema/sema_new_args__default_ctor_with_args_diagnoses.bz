// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/new_expr_args_test.cpp
//   (BEZEL_TEST sema_new_args, default_ctor_with_args_diagnoses).

module M; class T end T; class C public method M() _ := new T(1); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `new T` has 1 argument(s) but the implicit default constructor takes 0
// CHECK-SAME: (§22.7)
