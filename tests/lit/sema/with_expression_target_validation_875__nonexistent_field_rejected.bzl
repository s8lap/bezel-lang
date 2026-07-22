// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/with_expression_target_validation_875_test.cpp
//   (BEZEL_TEST with_expression_target_validation_875, nonexistent_field_rejected).
//
// §9.5, #876: a nonexistent field name is a compile error.
// `p with Z := 99 end` where Point has no Z must be rejected.

module M; record Point(X: int, Y: int); class C public static method Main(): int var p := new Point(3, 7); var q := p with(Z := 99); return q.X; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: field `Z` does not exist in record `Point` — `with` targets must be positional-field properties, auto-properties, or public fields
// CHECK-SAME: (§9.5, #876)
