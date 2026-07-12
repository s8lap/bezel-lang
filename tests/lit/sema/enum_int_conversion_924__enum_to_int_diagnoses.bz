// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/enum_int_conversion_924_test.cpp
//   (BEZEL_TEST enum_int_conversion_924, enum_to_int_diagnoses).
//
// §10: enum → underlying int is rejected.

module M; enum E A, B, C; end E; class C public static method Main(): int var e: E := E.C; var n: int := e; return n; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `n` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
