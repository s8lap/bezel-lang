// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/mutex_sendable_647_test.cpp
//   (BEZEL_TEST mutex_sendable_647, channel_lock_rejected_6_2_v0138_30_15).
//
// §30.15 / §3.6 / stdlib §6.2 v0.138: Channel<Lock> rejected.

module M; class C public static method Test() using var ch := new Channel<Lock>(); end Test; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `Lock` of `Channel<T>` is not sendable: the element type must satisfy the §30.14 sendable-type rules (not borrow-bearing, not move-only, not function-typed, not domain-local; class types must be transfer-closed/sealed)
// CHECK-SAME: (§30.14/§30.15, #600)
