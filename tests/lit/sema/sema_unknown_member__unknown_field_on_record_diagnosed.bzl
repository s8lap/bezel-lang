// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/unknown_member_test.cpp
//   (BEZEL_TEST sema_unknown_member, unknown_field_on_record_diagnosed).
//
// §9 record: an unknown field on a positional record.

module M; record Point(x: int, y: int); class C public method M(p: Point): int => p.z; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `z` on type `Point`
// CHECK-SAME: (§11.3, §12.1)
