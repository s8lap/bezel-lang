// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/send_600_channel_rules_test.cpp
//   (BEZEL_TEST send_600_channel_rules, owned_task_as_channel_element_rejected_30_14_stdlib).
//
// §30.14 stdlib table: OwningTask<T> is domain-local → not sendable.

module M; sealed class Result public Val: int := 0; destructor end destructor; end Result; class C public static method Test() using var ch := new Channel<OwningTask<Result>>(); end Test; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `OwningTask` of `Channel<T>` is not sendable: the element type must satisfy the §30.14 sendable-type rules (not borrow-bearing, not move-only, not function-typed, not domain-local; class types must be transfer-closed/sealed)
// CHECK-SAME: (§30.14/§30.15, #600)
