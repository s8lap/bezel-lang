// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/bool_ordering_1574_test.cpp
//   (BEZEL_TEST sema_bool_ordering_1574, le_on_bool_vars_rejected_spec_3_5).

module M; class C public method M(a: bool, b: bool) _ := a <= b; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `<=` is not defined for `bool` operands
// CHECK-SAME: (§3.5)
