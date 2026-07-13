// Ported from tests/sema/method_ref_type_test.cpp
//   (BEZEL_TEST sema_method_ref_type, method_ref_stored_in_func_typed_local_ok).
//
// §27.6 control 4: method reference stored in a correctly-typed local,
// then passed where the same function type is expected.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class H public static method Negate(x: int): int => -x; end H; class C public static method Apply(f: (int) -> int, n: int): int return f(n); end Apply; public static method Main(): int var neg: (int) -> int := H.Negate; return Apply(neg, 7); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
