// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/yield_in_lambda_747_test.cpp
//   (BEZEL_TEST yield_in_lambda_747, yield_return_in_lambda_rejected).
//
// §29.9: `yield return` inside a block-body lambda is rejected.

module M; static class F public static method Main(): int var g: () -> IEnumerable<int> := () => do yield return 1; end; return 0; end Main; end F;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `yield` is not allowed in a lambda — it is a method-level construct only
// CHECK-SAME: (§29.9)
