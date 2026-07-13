// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/call_arg_types_test.cpp
//   (BEZEL_TEST sema_argty, second_arg_mismatch_diagnoses).

module M; class C public method F(a: int, b: int) end F; public method M(flag: bool) this.F(1, flag); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the argument `b` to call to `F` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
