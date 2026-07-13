// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/named_arguments_test.cpp
//   (BEZEL_TEST sema_named_22_5, bound_argument_type_mismatch_diagnosed).
//
// §22.5 + §5.1: after binding, each bound argument is checked for
// assignability against the parameter it fills — here a `bool` named
// argument is bound to an `int` parameter, which §5.1 does not permit.

module M; class C public method F(a: int, b: int) end F; public method G() this.F(b := true, a := 1); end G; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the argument `b` to call to `F` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
