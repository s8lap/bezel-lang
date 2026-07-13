// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/unknown_member_test.cpp
//   (BEZEL_TEST sema_unknown_member, typo_of_real_method_diagnosed).
//
// §12.1: a typo of a real member (`Spea` for `Speak`) is unresolved → error.

module M; class Animal public method Speak(): int => 1; end Animal; class C public method M(a: Animal): int => a.Spea(); end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `Spea` on type `Animal`
// CHECK-SAME: (§11.3, §12.1)
