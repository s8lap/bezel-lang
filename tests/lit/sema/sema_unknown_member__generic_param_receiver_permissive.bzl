// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/unknown_member_test.cpp
//   (BEZEL_TEST sema_unknown_member, generic_param_receiver_permissive).
//
// §14.2 / fix #1078: a method call on an unconstrained generic type parameter
// `T` is now a sema error — the call has no constraint backing and no member
// surface (§14.2 permits only Object members on unconstrained T). Prior to
// fix #1078 this was silently accepted and miscompiled to 0 at runtime.

module M; class Box<T> public method M(t: T): int var s := t.Whatever(); return 0; end M; end Box;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `Whatever` on unconstrained type parameter `T`; add a `where T: <constraint>` that provides this member
// CHECK-SAME: (§14.2)
