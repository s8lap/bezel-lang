// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/range_literal_step_zero_1628_test.cpp
//   (BEZEL_TEST sema_range_literal_step_zero_1628, bare_range_local_literal_step_zero_rejected_spec_16_4).
//
// §16.4: the rule applies "at construction", not only inside a `for` —
// a bare Range-valued local with a literal step-0 must also be rejected.

module M; class C public static method M(): int var r := 0..10 step 0; return 0; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a range step of zero is a compile error
// CHECK-SAME: (§16.4)
