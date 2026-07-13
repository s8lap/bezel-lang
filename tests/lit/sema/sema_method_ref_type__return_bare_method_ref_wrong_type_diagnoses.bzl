// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/method_ref_type_test.cpp
//   (BEZEL_TEST sema_method_ref_type, return_bare_method_ref_wrong_type_diagnoses).
//
// §27.6 + §5.1 example 1: `return a.GetVal;` where return type is int.
// The method reference `a.GetVal` has type `() -> int`; that is NOT an int.
// This must produce a compile error — not silent malformed IR.

module M; class A public method GetVal(): int => 42; end A; class C public static method Foo(a: A): int return a.GetVal; end Foo; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the returned value has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
