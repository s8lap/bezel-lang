// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/unknown_member_test.cpp
//   (BEZEL_TEST sema_unknown_member, unknown_member_on_this_diagnosed).
//
// §11.3: `this.<unknown>` — the receiver is the current user class.

module M; class C private _x: int; public method M(): int => this.bogus; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `bogus` on type `C`
// CHECK-SAME: (§11.3, §12.1)
