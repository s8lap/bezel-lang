// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/bool_ordering_1574_test.cpp
//   (BEZEL_TEST sema_bool_ordering_1574, lt_on_bool_literals_rejected_spec_3_5).
//
// The reported bug: `true < false` compiled and evaluated `true`. Confirm
// the literal form is rejected too (not just typed-parameter operands).

module M; class C public method M() _ := true < false; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `<` is not defined for `bool` operands
// CHECK-SAME: (§3.5)
