// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/variance_position_test.cpp
//   (BEZEL_TEST sema_variance_marker_scope, spec_11_8_static_operator_out_rejected).
//
// §11.8: an operator overload is implicitly static (A.4 `OperatorDecl ::=
// Visibility "static" "method" "operator" ...`), so it triggers the same
// rejection as an explicit `static method`.

module M; interface INumber2<out T> where T: INumber2<T> static operator +(a: T, b: T): T; end INumber2;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: interface `INumber2` declares a static member, so its type parameter `T` may not be marked `out`
// CHECK-SAME: (§11.8)
