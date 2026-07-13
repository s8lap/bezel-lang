// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/send_600_channel_rules_test.cpp
//   (BEZEL_TEST send_600_channel_rules, channel_borrow_bearing_record_element_rejected_30_15).
//
// §30.15 / §3.6: Channel<BorrowRec> where BorrowRec is borrow-bearing → rejected.

module M; class Node end Node; record BorrowRec public Ref: Node?; end BorrowRec; class C public static method Test() using var ch := new Channel<BorrowRec>(); end Test; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `BorrowRec` of `Channel<T>` is not sendable: the element type must satisfy the §30.14 sendable-type rules (not borrow-bearing, not move-only, not function-typed, not domain-local; class types must be transfer-closed/sealed)
// CHECK-SAME: (§30.14/§30.15, #600)
