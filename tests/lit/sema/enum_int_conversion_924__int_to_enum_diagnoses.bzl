// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/enum_int_conversion_924_test.cpp
//   (BEZEL_TEST enum_int_conversion_924, int_to_enum_diagnoses).
//
// §10: int → enum is rejected.

module M; enum E A, B, C; end E; class C public static method Main(): int var n: int := 2; var e: E := n; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `e` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
