// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/abstract_impl_test.cpp
//   (BEZEL_TEST sema_abs_impl, unimplemented_abstract_method_diagnoses).

module M; abstract class Base public abstract method Draw(); end Base; class C : Base end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: concrete class `C` does not implement inherited abstract member `Draw`
// CHECK-SAME: (§12.5)
