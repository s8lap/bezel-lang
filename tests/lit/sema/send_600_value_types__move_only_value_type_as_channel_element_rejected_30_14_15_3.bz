// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/send_600_sendability_value_types_test.cpp
//   (BEZEL_TEST send_600_value_types, move_only_value_type_as_channel_element_rejected_30_14_15_3).
//
// §30.14/§15.3: move-only value types ([N owned]T) are NOT sendable.
// Channel<[2 owned]Node> must be rejected.

module M; sealed class Node public Val: int := 0; destructor end destructor; end Node; class C public static method Test() using var ch := new Channel<[2 owned]Node>(); end Test; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `[N owned]T` of `Channel<T>` is not sendable: the element type must satisfy the §30.14 sendable-type rules (not borrow-bearing, not move-only, not function-typed, not domain-local; class types must be transfer-closed/sealed)
// CHECK-SAME: (§30.14/§30.15, #600)
