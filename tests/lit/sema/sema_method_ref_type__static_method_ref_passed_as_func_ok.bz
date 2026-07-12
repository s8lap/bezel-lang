// Ported from tests/sema/method_ref_type_test.cpp
//   (BEZEL_TEST sema_method_ref_type, static_method_ref_passed_as_func_ok).
//
// §27.6 control 3: static method reference passed where `(int) -> int` is
// expected. This is the existing method-reference codegen path; must not
// be broken by the new function-type check.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class H public static method Square(x: int): int => x * x; end H; class C public static method Apply(f: (int) -> int, n: int): int return f(n); end Apply; public static method Main(): int return Apply(H.Square, 5); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
