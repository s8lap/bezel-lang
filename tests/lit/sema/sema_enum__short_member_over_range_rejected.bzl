// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/enum_restrictions_test.cpp
//   (BEZEL_TEST sema_enum, short_member_over_range_rejected).
//
// §10.3: a value that overflows a `short` (but fits `int`) is rejected.

module M; enum E: short A := 40000; end E;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: enum member `A` value 40000 does not fit the underlying type `short`
// CHECK-SAME: (§10.3 — all declared values must fit the underlying type's range)
