// Ported from tests/sema/method_ref_type_test.cpp
//   (BEZEL_TEST sema_method_ref_type, var_func_typed_local_from_method_ref_ok).
//
// §27.6 control 1: `var f: () -> int := a.GetVal;` — assigns a method reference
// to a function-typed local. This is the canonical idiom and must NOT error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class A public method GetVal(): int => 42; end A; class C public static method Foo(a: A): int var f: () -> int := a.GetVal; return f(); end Foo; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
