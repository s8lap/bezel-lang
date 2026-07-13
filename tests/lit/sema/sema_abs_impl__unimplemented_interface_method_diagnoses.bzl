// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/abstract_impl_test.cpp
//   (BEZEL_TEST sema_abs_impl, unimplemented_interface_method_diagnoses).

module M; interface IDoer method Do(); end IDoer; class C : IDoer end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: concrete class `C` does not implement inherited abstract member `Do`
// CHECK-SAME: (§12.5)
