// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/call_arg_types_test.cpp
//   (BEZEL_TEST sema_argty, string_arg_to_int_param_diagnoses).

module M; class C public method F(a: int) end F; public method M() this.F("hi"); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the argument `a` to call to `F` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
