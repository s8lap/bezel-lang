// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/inline_array_element_count_test.cpp
//   (BEZEL_TEST inline_array_element_count, zero_size_via_const_rejected).
//
// REJECT: a zero-sized inline array via a `const` size.

module M; class C public const Z: int := 0; public static method Main(): int var a: [Z]int; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: an inline-array size must be greater than zero — `[0]T` is not a valid type
// CHECK-SAME: (§15.1)
