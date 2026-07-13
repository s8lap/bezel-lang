// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/sealed_member_override_485_test.cpp
//   (BEZEL_TEST sema_sealed_member_485, override_of_nonvirtual_still_gives_12_1_diagnostic).
//
// §12.1 regression guard — test 5: `override` targeting a genuinely non-virtual
// base member must still emit the §12.1 "not virtual/abstract/override" message
// (the original §12.1 code path must not be disturbed).

module M; class Base public method Speak() _ := 1; end Speak; end Base; class Derived : Base public override method Speak() _ := 2; end Speak; end Derived;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `Speak` is declared `override` but the base member it would override is not `virtual`, `abstract`, or `override`
// CHECK-SAME: (§12.1)
