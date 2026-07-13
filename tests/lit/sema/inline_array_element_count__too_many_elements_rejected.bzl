// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/inline_array_element_count_test.cpp
//   (BEZEL_TEST inline_array_element_count, too_many_elements_rejected).
//
// §15.4: too MANY elements is a compile error (was: runtime SIGSEGV).

module M; class C public static method Main(): int var a: [3]int := [1, 2, 3, 4, 5]; return a[0]; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: inline-array initializer has 5 element(s) but the type requires exactly 3 (`[3]…`)
// CHECK-SAME: (§15.4)
