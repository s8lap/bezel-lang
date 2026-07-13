// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/method_ref_type_test.cpp
//   (BEZEL_TEST sema_method_ref_type, local_init_bare_method_ref_wrong_type_diagnoses).
//
// §27.6 + §5.1 example 2: `var x: int := a.GetVal;` — same mismatch.
// Assigning a `() -> int` to an `int` local is not assignable.

module M; class A public method GetVal(): int => 42; end A; class C public static method Foo(a: A) var x: int := a.GetVal; end Foo; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `x` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
