// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/abstract_instantiation_test.cpp
//   (BEZEL_TEST sema_inst, new_abstract_class_diagnoses).

module M; abstract class A end A; class C public method M() _ := new A(); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot instantiate abstract class `A` with `new`
// CHECK-SAME: (§12.5)
