// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/method_ref_type_test.cpp
//   (BEZEL_TEST sema_method_ref_type, assign_bare_method_ref_to_int_local_diagnoses).
//
// §27.6 + §5.1 example 3: passing `a.GetVal` where an `int` parameter is
// expected. The function type `() -> int` is not assignable to `int`.
// (Argument type checking — tested via return to keep the repro simple.)

module M; class A public method GetVal(): int => 42; end A; class C public static method Foo(a: A) var x: int := 0; x := a.GetVal; end Foo; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the assigned value has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
