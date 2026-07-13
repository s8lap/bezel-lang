// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/named_arguments_test.cpp
//   (BEZEL_TEST sema_named_22_5, duplicate_binding_positional_then_named_diagnosed).
//
// §22.5: a parameter may be bound at most once — here `a` is filled both
// positionally and again by name.

module M; class C public method F(a: int, b: int) end F; public method G() this.F(1, a := 2); end G; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: call to `F`: parameter `a` is bound more than once
// CHECK-SAME: (§22.5)
