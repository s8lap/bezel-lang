// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/abstract_instantiation_test.cpp
//   (BEZEL_TEST sema_inst, new_static_class_diagnoses).

module M; static class S end S; class C public method M() _ := new S(); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot instantiate static class `S` with `new`
// CHECK-SAME: (§11.7)
