// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/send_600_sendability_class_types_test.cpp
//   (BEZEL_TEST send_600_class_types, sealed_class_with_non_owned_class_field_rejected_30_14_rule2).
//
// §30.14 Rule 2: sealed class with a non-owned class-typed field (a
// back-reference / borrow alias) is not sendable.

module M; sealed class Node public Val: int := 0; destructor end destructor; end Node; sealed class Tree public Parent: Node?; public Val: int := 0; destructor end destructor; end Tree; class C public static method Test() using var ch := new Channel<Tree>(); end Test; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `Tree` of `Channel<T>` is not sendable: the element type must satisfy the §30.14 sendable-type rules (not borrow-bearing, not move-only, not function-typed, not domain-local; class types must be transfer-closed/sealed)
// CHECK-SAME: (§30.14/§30.15, #600)
