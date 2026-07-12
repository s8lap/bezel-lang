// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/new_expr_args_test.cpp
//   (BEZEL_TEST sema_new_args, wrong_arg_type_diagnoses).

module M; class T public constructor(a: int) end constructor; end T; class C public method M(flag: bool) _ := new T(flag); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the argument `a` to `new T` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
