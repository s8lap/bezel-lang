// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/unknown_member_test.cpp
//   (BEZEL_TEST sema_unknown_member, unknown_static_member_on_type_diagnosed).
//
// §11.3: an unknown STATIC member accessed via the type name.

module M; class Other public static _total: int := 5; end Other; class C public method M(): int => Other.NoSuchStatic; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `NoSuchStatic` on type `Other`
// CHECK-SAME: (§11.3, §12.1)
