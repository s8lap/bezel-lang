// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/inline_array_element_count_test.cpp
//   (BEZEL_TEST inline_array_element_count, too_few_elements_rejected).
//
// §15.4: too FEW elements is a compile error (was: garbage reads).

module M; class C public static method Main(): int var a: [5]int := [1, 2, 3]; return a[0]; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: inline-array initializer has 3 element(s) but the type requires exactly 5 (`[5]…`)
// CHECK-SAME: (§15.4)
