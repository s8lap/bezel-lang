// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/hiding_680_primary_ctor_param_test.cpp
//   (BEZEL_TEST sema_680_primary_ctor_hiding, method_hiding_base_method_still_diagnosed).
//
// §12.2 / #680 regression: a method hiding a base method must STILL diagnose.

module M; class Base public method Speak() _ := 1; end Speak; end Base; class Derived : Base public method Speak() _ := 2; end Speak; end Derived;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member `Speak` hides an inherited member of the same name; hiding is forbidden — use `override` for a virtual base member, or choose a different name
// CHECK-SAME: (§12.2)
