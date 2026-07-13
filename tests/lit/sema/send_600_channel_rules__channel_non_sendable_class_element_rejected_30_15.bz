// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/send_600_channel_rules_test.cpp
//   (BEZEL_TEST send_600_channel_rules, channel_non_sendable_class_element_rejected_30_15).
//
// §30.15 / §3.6: Channel<NonSendableClass> where the class is open → rejected.

module M; class OpenData public Val: int := 0; destructor end destructor; end OpenData; class C public static method Test() using var ch := new Channel<OpenData>(); end Test; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `OpenData` of `Channel<T>` is not sendable: the element type must satisfy the §30.14 sendable-type rules (not borrow-bearing, not move-only, not function-typed, not domain-local; class types must be transfer-closed/sealed)
// CHECK-SAME: (§30.14/§30.15, #600)
