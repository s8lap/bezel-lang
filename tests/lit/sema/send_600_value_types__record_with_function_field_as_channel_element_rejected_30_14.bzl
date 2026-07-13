// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/send_600_sendability_value_types_test.cpp
//   (BEZEL_TEST send_600_value_types, record_with_function_field_as_channel_element_rejected_30_14).
//
// §30.14: function-typed values are never sendable ("Function-typed values are
// never sendable"). A record with a function field is borrow-bearing → rejected.

module M; record FuncRec public Callback: (int) -> int; end FuncRec; class C public static method Test() using var ch := new Channel<FuncRec>(); end Test; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `FuncRec` of `Channel<T>` is not sendable: the element type must satisfy the §30.14 sendable-type rules (not borrow-bearing, not move-only, not function-typed, not domain-local; class types must be transfer-closed/sealed)
// CHECK-SAME: (§30.14/§30.15, #600)
