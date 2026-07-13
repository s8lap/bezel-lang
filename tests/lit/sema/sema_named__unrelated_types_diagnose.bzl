// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/named_assignability_test.cpp
//   (BEZEL_TEST sema_named, unrelated_types_diagnose).

module M; class A end A; class B end B; class C public method M() var a: A := new B(); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `a` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
