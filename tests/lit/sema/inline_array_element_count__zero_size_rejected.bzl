// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/inline_array_element_count_test.cpp
//   (BEZEL_TEST inline_array_element_count, zero_size_rejected).
//
// REJECT: a zero-sized inline array (no initializer).

module M; class C public static method Main(): int var a: [0]int; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: an inline-array size must be greater than zero — `[0]T` is not a valid type
// CHECK-SAME: (§15.1)
