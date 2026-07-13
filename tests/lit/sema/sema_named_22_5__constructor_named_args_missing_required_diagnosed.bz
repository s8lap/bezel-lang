// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/named_arguments_test.cpp
//   (BEZEL_TEST sema_named_22_5, constructor_named_args_missing_required_diagnosed).
//
// §22.5 / §22.7: a constructor named-arg call that leaves a required
// parameter unfilled is diagnosed.

module M; class P public X: int := 0; public Y: int := 0; public constructor(x: int, y: int) X := x; Y := y; end constructor; end P; class C public method G() var p := new P(x := 10); end G; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `new P`: required parameter `y` is not supplied
// CHECK-SAME: (§22.5)
