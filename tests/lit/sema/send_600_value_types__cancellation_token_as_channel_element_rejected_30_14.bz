// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/send_600_sendability_value_types_test.cpp
//   (BEZEL_TEST send_600_value_types, cancellation_token_as_channel_element_rejected_30_14).
//
// §30.14: CancellationToken is explicitly listed as borrow-bearing record
// → not sendable (spec §30.14: "CancellationToken is a borrow-bearing record
// and therefore not sendable by the value-type rule").
// Channel<CancellationToken> must be rejected.

module M; class C public static method Test() using var ch := new Channel<CancellationToken>(); end Test; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `CancellationToken` of `Channel<T>` is not sendable: the element type must satisfy the §30.14 sendable-type rules (not borrow-bearing, not move-only, not function-typed, not domain-local; class types must be transfer-closed/sealed)
// CHECK-SAME: (§30.14/§30.15, #600)
