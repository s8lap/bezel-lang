// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/enum_restrictions_test.cpp
//   (BEZEL_TEST sema_enum, byte_member_over_255_rejected).
//
// §10.3 (#740): a member value that exceeds the declared underlying type's
// range is a compile error (it previously wrapped silently at codegen).

module M; enum E: byte A := 256; end E;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: enum member `A` value 256 does not fit the underlying type `byte`
// CHECK-SAME: (§10.3 — all declared values must fit the underlying type's range)
