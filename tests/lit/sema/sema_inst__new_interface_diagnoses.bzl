// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/abstract_instantiation_test.cpp
//   (BEZEL_TEST sema_inst, new_interface_diagnoses).

module M; interface I method M(); end I; class C public method M() _ := new I(); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot instantiate interface `I` with `new`
// CHECK-SAME: (§3.1 / §3.2 / §17.7)
