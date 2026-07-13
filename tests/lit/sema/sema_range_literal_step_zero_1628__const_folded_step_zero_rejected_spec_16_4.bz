// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/range_literal_step_zero_1628_test.cpp
//   (BEZEL_TEST sema_range_literal_step_zero_1628, const_folded_step_zero_rejected_spec_16_4).
//
// §16.4 "constant after folding": a step expression that is not a bare `0`
// literal but const-folds to zero (`2 - 2`) is still a compile-time
// constant and must be rejected the same as a bare literal — mirroring the
// identical "constants or constant after folding" discipline the spec uses
// for the sibling direction/sign-mismatch warning in the same section.

module M; class C public static method M(): int for var i in 0..10 step (2 - 2) do end; return 0; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a range step of zero is a compile error
// CHECK-SAME: (§16.4)
