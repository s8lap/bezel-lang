// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/send_600_sendability_class_types_test.cpp
//   (BEZEL_TEST send_600_class_types, closed_class_one_non_sendable_subclass_rejected_30_14).
//
// §30.14: `closed` hierarchy where ONE subclass is not sendable
// (has a non-owned class field) → the whole hierarchy is not sendable.

module M; closed class Shape public abstract method Area(): int; end Shape; sealed class GoodCircle : Shape public R: int := 0; public override method Area(): int => 0; destructor end destructor; end GoodCircle; class ExternalRef end ExternalRef; sealed class BadRect : Shape public W: int := 0; public Ref: ExternalRef?; public override method Area(): int => 0; destructor end destructor; end BadRect; class C public static method Test() using var ch := new Channel<Shape>(); end Test; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `Shape` of `Channel<T>` is not sendable: the element type must satisfy the §30.14 sendable-type rules (not borrow-bearing, not move-only, not function-typed, not domain-local; class types must be transfer-closed/sealed)
// CHECK-SAME: (§30.14/§30.15, #600)
