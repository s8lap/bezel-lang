// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/inline_array_element_count_test.cpp
//   (BEZEL_TEST inline_array_element_count, multi_dimensional_rejected).
//
// §15.1 (#953): an inline array may not be the element of another inline array
// (`[N][M]T`) — multi-dimensional inline arrays are not admitted.

module M; class C public static method Main(): int var m: [2][3]int; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: an inline array may not be the element of another inline array (`[N][M]T`) — multi-dimensional inline arrays are not admitted
// CHECK-SAME: (§15.1)
