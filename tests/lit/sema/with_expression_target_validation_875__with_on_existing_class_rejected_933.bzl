// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/with_expression_target_validation_875_test.cpp
//   (BEZEL_TEST with_expression_target_validation_875, with_on_existing_class_rejected_933).
//
// REJECT: `with` on an existing class variable (#933) — unaffected by
// v0.188, still rejected, still under §9.5.

module M; class Box public property V: int; public constructor() V := 0; end constructor; destructor end destructor; end Box; class C public static method Main(): int using var b1 := new Box(); b1.V := 7; var b2 := b1 with(V := 42); return b2.V; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `with` requires a record-typed source; this source is class-typed
// CHECK-SAME: (§9.5, v0.188)
