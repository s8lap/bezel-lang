// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/send_600_sendability_class_types_test.cpp
//   (BEZEL_TEST send_600_class_types, sealed_class_with_owned_non_sendable_field_rejected_30_14).
//
// §30.14 Rule 2: sealed class with an `owned` field whose type is a NON-sendable
// class (open inner) → the outer is also not sendable (transitivity).

module M; class OpenInner public Val: int := 0; destructor end destructor; end OpenInner; sealed class Outer public owned Child: OpenInner := new OpenInner(); destructor end destructor; end Outer; class C public static method Test() using var ch := new Channel<Outer>(); end Test; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `Outer` of `Channel<T>` is not sendable: the element type must satisfy the §30.14 sendable-type rules (not borrow-bearing, not move-only, not function-typed, not domain-local; class types must be transfer-closed/sealed)
// CHECK-SAME: (§30.14/§30.15, #600)
