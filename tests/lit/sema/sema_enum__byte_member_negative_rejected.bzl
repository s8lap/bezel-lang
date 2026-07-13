// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/enum_restrictions_test.cpp
//   (BEZEL_TEST sema_enum, byte_member_negative_rejected).
//
// §10.3: an unsigned underlying type rejects a negative value.

module M; enum E: byte A := -1; end E;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: enum member `A` value -1 does not fit the underlying type `byte`
// CHECK-SAME: (§10.3 — all declared values must fit the underlying type's range)
