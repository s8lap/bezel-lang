// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/named_arguments_test.cpp
//   (BEZEL_TEST sema_named_22_5, missing_required_parameter_diagnosed).
//
// §22.5: every required (non-default) parameter must be supplied; binding
// only `a` leaves required `b` unfilled.

module M; class C public method F(a: int, b: int) end F; public method G() this.F(a := 1); end G; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: call to `F`: required parameter `b` is not supplied
// CHECK-SAME: (§22.5)
