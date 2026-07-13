// Ported from tests/sema/compound_assign_cross_family_1689_test.cpp
//   (BEZEL_TEST compound_assign_cross_family_1689, float_plus_float_literal_accepted).
//
// `f += 2.5f;` — the float suffix keeps the RHS in the float family (R = T
// directly), unlike the unsuffixed-double negative case above.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var f: float := 1.0f; f += 2.5f; return int(f); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
