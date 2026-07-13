// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_type_args_all_or_nothing_1687_test.cpp
//   (BEZEL_TEST generic_type_args_1687, class_ctor_no_type_args_rejected).
//
// §14.5 Scope / #1687: the same rule applies to a generic CLASS constructor.

module M; class Box<T> private _v: T; public constructor(v: T) _v := v; end constructor; end Box; class C public static method Main(): int var b := new Box(5); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `new Box(...)` supplies no type arguments, but `Box` declares 1 type parameter(s) — class-, record-, and interface-level type arguments are never inferred from constructor arguments; supply the full explicit list (`new Box<...>(...)`) or, where a target type exists, use the `new()` shorthand
// CHECK-SAME: (§14.5, §17.7)
