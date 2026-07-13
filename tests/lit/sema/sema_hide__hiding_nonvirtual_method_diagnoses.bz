// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/hiding_override_test.cpp
//   (BEZEL_TEST sema_hide, hiding_nonvirtual_method_diagnoses).

module M; class Base public method Speak() _ := 1; end Speak; end Base; class Derived : Base public method Speak() _ := 2; end Speak; end Derived;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member `Speak` hides an inherited member of the same name; hiding is forbidden — use `override` for a virtual base member, or choose a different name
// CHECK-SAME: (§12.2)
