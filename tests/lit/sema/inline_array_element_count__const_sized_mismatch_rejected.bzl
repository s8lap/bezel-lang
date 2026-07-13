// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/inline_array_element_count_test.cpp
//   (BEZEL_TEST inline_array_element_count, const_sized_mismatch_rejected).
//
// §15.1: a `const`-folded size with the wrong element count is rejected.

module M; class C public const N: int := 4; public static method Main(): int var a: [N]int := [7, 8, 9]; return a[0]; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: inline-array initializer has 3 element(s) but the type requires exactly 4 (`[4]…`)
// CHECK-SAME: (§15.4)
