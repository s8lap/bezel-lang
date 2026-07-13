// Ported from tests/sema/compound_assign_cross_family_1689_test.cpp
//   (BEZEL_TEST compound_assign_cross_family_1689, decimal_plus_int_and_decimal_accepted).
//
// `decimal += <int literal>` and `decimal += <decimal literal>` both keep
// R = T (an int widens into decimal per §6.2) and must stay accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var d: decimal := 1.0m; d += 5; d += 2m; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
