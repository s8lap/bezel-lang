// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/hiding_override_test.cpp
//   (BEZEL_TEST sema_hide, override_of_nonvirtual_diagnoses).

module M; class Base public method Speak() _ := 1; end Speak; end Base; class Derived : Base public override method Speak() _ := 2; end Speak; end Derived;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `Speak` is declared `override` but the base member it would override is not `virtual`, `abstract`, or `override`
// CHECK-SAME: (§12.1)
