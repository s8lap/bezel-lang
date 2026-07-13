// Ported from tests/sema/generic_inconsistent_type_arg_896_test.cpp
//   (BEZEL_TEST generic_inconsistent_type_arg_896, two_type_params_accepted).
//
// §14.1 / #896: a two-type-parameter method Pair<A,B>(a:A, b:B) given
// (int, bool) — A=int, B=bool, no conflict. ACCEPT.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; static class Q public static method Pair<A, B>(a: A, b: B): int return 0; end Pair; public static method Main(): int var x: int := 1; var y: bool := true; return Pair(x, y); end Main; end Q;

// NODIAG-NOT: error
// NODIAG-NOT: §
