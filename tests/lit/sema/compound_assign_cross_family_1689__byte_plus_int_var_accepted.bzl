// Ported from tests/sema/compound_assign_cross_family_1689_test.cpp
//   (BEZEL_TEST compound_assign_cross_family_1689, byte_plus_int_var_accepted).
//
// `acc += n;` where `acc: byte`, `n: int` — the spec's own "admitted: R =
// int; checked write-back at runtime" example.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var n: int := 2; var acc: byte := 40; acc += n; return int(acc); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
