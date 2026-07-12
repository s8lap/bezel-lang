// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/named_assignability_test.cpp
//   (BEZEL_TEST sema_named, base_to_derived_diagnoses).

module M; class Base end Base; class Derived : Base end Derived; class C public method M() var d: Derived := new Base(); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `d` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
