// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/unknown_member_test.cpp
//   (BEZEL_TEST sema_unknown_member, unknown_constant_on_enum_diagnosed).
//
// §10 enum: an unknown member name accessed statically on the enum type.

module M; enum Color Red, Green, Blue; end Color; class C public method M(): int var c := Color.Purple; return 0; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `Purple` on type `Color`
// CHECK-SAME: (§11.3, §12.1)
