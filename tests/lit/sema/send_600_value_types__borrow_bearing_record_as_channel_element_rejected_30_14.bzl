// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/send_600_sendability_value_types_test.cpp
//   (BEZEL_TEST send_600_value_types, borrow_bearing_record_as_channel_element_rejected_30_14).
//
// §30.14: a record with a class-typed (reference-typed) field is borrow-bearing
// (§9.7) → NOT sendable → Channel<BorrowRec> is rejected with "not sendable".

module M; class Node end Node; record BorrowRec public Ref: Node?; end BorrowRec; class C public static method Test() using var ch := new Channel<BorrowRec>(); end Test; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `BorrowRec` of `Channel<T>` is not sendable: the element type must satisfy the §30.14 sendable-type rules (not borrow-bearing, not move-only, not function-typed, not domain-local; class types must be transfer-closed/sealed)
// CHECK-SAME: (§30.14/§30.15, #600)
