// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_method_constraint_callsite_746_test.cpp
//   (BEZEL_TEST method_constraint_callsite_746, unconstrained_class_field_t_method_rejected_1078).
//
// §14.2 (#1078): a method call on an unconstrained class-level T field is
// rejected — T provides no member `Value`, so this is ill-formed.

module M; class Holder<T> private _item: T?; public method GetValue(): int _item!.Value(); return 0; end GetValue; end Holder; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `Value` on unconstrained type parameter `T`; add a `where T: <constraint>` that provides this member
// CHECK-SAME: (§14.2)
