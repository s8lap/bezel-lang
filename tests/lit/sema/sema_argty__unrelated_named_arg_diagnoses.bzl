// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/call_arg_types_test.cpp
//   (BEZEL_TEST sema_argty, unrelated_named_arg_diagnoses).

module M; class A end A; class B end B; class C public method F(a: A) end F; public method M() this.F(new B()); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the argument `a` to call to `F` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
