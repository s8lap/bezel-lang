// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/sealed_member_override_485_test.cpp
//   (BEZEL_TEST sema_sealed_member_485, sealed_class_derivation_still_rejected_12_4).
//
// §12.4 regression guard — test 6: deriving from a sealed class is still
// rejected (whole-class sealing, unchanged from before).

module M; sealed class Base end Base; class Derived : Base end Derived;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot inherit from sealed class `Base`
// CHECK-SAME: (§12.4)
