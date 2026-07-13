// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/new_type_param_constraint_992_test.cpp
//   (BEZEL_TEST new_type_param_constraint_992, reject_class_level_missing_new_constraint).
//
// §14.1 (#992): `new T()` in a class-level method where the class declares
// only `where T: class` (no `new()`) must be rejected.

module M; class Widget public static total_ctors: int := 0; public constructor(v: int) Widget.total_ctors := Widget.total_ctors + 1; end constructor; destructor end destructor; end Widget; class Box<T> where T: class public method MakeOne(): T return new T(); end MakeOne; end Box; class C public static method Main(): int using var b := new Box<Widget>(); var w := b.MakeOne(); return Widget.total_ctors; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `new T()` requires the type parameter `T` to be constrained `where T: new()` — the body type-checks under the weakest assumption its constraints establish, and construction is not guaranteed without the `new()` constraint
// CHECK-SAME: (§14.1, #992)
