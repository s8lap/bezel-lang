// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/shared_sync_sendable_689_test.cpp
//   (BEZEL_TEST shared_sync_sendable_689, channel_shared_string_rejected_28_13_v0140_30_15).
//
// §30.15 / §3.6 / §28.13 v0.140: Channel<Shared<string>> rejected.

module M; class C public static method Test() using var ch := new Channel<Shared<string>>(); end Test; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `Shared` of `Channel<T>` is not sendable: the element type must satisfy the §30.14 sendable-type rules (not borrow-bearing, not move-only, not function-typed, not domain-local; class types must be transfer-closed/sealed)
// CHECK-SAME: (§30.14/§30.15, #600)
