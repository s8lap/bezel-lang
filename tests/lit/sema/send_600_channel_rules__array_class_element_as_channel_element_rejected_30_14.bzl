// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/send_600_channel_rules_test.cpp
//   (BEZEL_TEST send_600_channel_rules, array_class_element_as_channel_element_rejected_30_14).
//
// §30.14 stdlib table: Array<SomeClass> carries borrows → not sendable.

module M; sealed class Item public Val: int := 0; destructor end destructor; end Item; class C public static method Test() using var ch := new Channel<Array<Item>>(); end Test; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `Array` of `Channel<T>` is not sendable: the element type must satisfy the §30.14 sendable-type rules (not borrow-bearing, not move-only, not function-typed, not domain-local; class types must be transfer-closed/sealed)
// CHECK-SAME: (§30.14/§30.15, #600)
