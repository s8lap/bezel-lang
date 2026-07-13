// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/send_600_channel_rules_test.cpp
//   (BEZEL_TEST send_600_channel_rules, shared_t_as_channel_element_rejected_28_13_30_14).
//
// §30.14 / §28.13: Shared<T> is domain-local (ref-count not synchronized) →
// not sendable. Channel<Shared<SomeClass>> must be rejected.

module M; sealed class Obj public Val: int := 0; destructor end destructor; end Obj; class C public static method Test() using var ch := new Channel<Shared<Obj>>(); end Test; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `Shared` of `Channel<T>` is not sendable: the element type must satisfy the §30.14 sendable-type rules (not borrow-bearing, not move-only, not function-typed, not domain-local; class types must be transfer-closed/sealed)
// CHECK-SAME: (§30.14/§30.15, #600)
