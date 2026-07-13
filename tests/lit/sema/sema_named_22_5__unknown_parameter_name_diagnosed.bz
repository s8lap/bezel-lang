// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/named_arguments_test.cpp
//   (BEZEL_TEST sema_named_22_5, unknown_parameter_name_diagnosed).
//
// §22.5: a named argument that names no parameter is an error.

module M; class C public method F(a: int, b: int) end F; public method G() this.F(a := 1, q := 2); end G; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: call to `F` has no parameter named `q`
// CHECK-SAME: (§22.5)
