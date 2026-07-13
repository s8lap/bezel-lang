// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/enum_restrictions_test.cpp
//   (BEZEL_TEST sema_enum, auto_increment_overflow_rejected).
//
// §10.3: auto-increment that overflows the underlying type is rejected
// (`byte X := 255, Y;` → Y is 256, out of range).

module M; enum E: byte X := 255, Y; end E;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: enum member `Y` value 256 does not fit the underlying type `byte`
// CHECK-SAME: (§10.3 — all declared values must fit the underlying type's range)
