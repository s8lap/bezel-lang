// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/range_literal_step_zero_1628_test.cpp
//   (BEZEL_TEST sema_range_literal_step_zero_1628, for_loop_literal_step_zero_rejected_spec_16_4).
//
// §16.4: `for var i in 0..10 step 0` — the exact issue #1628 repro. A
// literal step of zero inside a `for` iterable must be a compile error.

module M; class C public static method Main(): int var sum: int := 0; for var i in 0..10 step 0 do sum := sum + i; end; return sum; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a range step of zero is a compile error
// CHECK-SAME: (§16.4)
