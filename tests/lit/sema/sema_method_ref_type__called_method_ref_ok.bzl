// Ported from tests/sema/method_ref_type_test.cpp
//   (BEZEL_TEST sema_method_ref_type, called_method_ref_ok).
//
// §27.6 control 2: `a.GetVal()` (called) — must NOT error; returns int.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class A public method GetVal(): int => 42; end A; class C public static method Foo(a: A): int return a.GetVal(); end Foo; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
