// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/with_expression_target_validation_875_test.cpp
//   (BEZEL_TEST with_expression_target_validation_875, with_on_new_class_rejected_9_5_v0188).
//
// REJECT (v0.188, was ACCEPT pre-v0.188 #933): the former object-initializer
// form on a `new` class expression is now rejected too — the discriminator
// is the source's TYPE KIND (class), never the presence of `new` (§9.5).
// This is the exact spec §9.5 example: "new Point(1, 2) with Y := 9 end has
// a record-typed source ... the same spelling on a class-typed new
// expression is rejected."

module M; class Box public property V: int; public constructor() V := 0; end constructor; destructor end destructor; end Box; class C public static method Main(): int using var b := new Box() with(V := 42); return b.V; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `with` requires a record-typed source; this source is class-typed
// CHECK-SAME: (§9.5, v0.188)
