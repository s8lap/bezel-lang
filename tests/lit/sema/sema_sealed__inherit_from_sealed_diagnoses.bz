// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/sealed_class_test.cpp
//   (BEZEL_TEST sema_sealed, inherit_from_sealed_diagnoses).

module M; sealed class Base end Base; class Derived : Base end Derived;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot inherit from sealed class `Base`
// CHECK-SAME: (§12.4)
