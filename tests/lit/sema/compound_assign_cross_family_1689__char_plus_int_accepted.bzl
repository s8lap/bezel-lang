// Ported from tests/sema/compound_assign_cross_family_1689_test.cpp
//   (BEZEL_TEST compound_assign_cross_family_1689, char_plus_int_accepted).
//
// `c += 1;` where `c: char` — the §5.2 code-point checked-narrowing case:
// `char` is named explicitly alongside the true integer types in §6.6's
// write-back rule ("T is an integer type or `char`").
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var c: char := 'a'; c += 1; return int(c); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
