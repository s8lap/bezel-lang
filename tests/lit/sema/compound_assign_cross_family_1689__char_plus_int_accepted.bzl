// Ported from tests/sema/compound_assign_cross_family_1689_test.cpp
//   (BEZEL_TEST compound_assign_cross_family_1689, char_plus_int_accepted).
//
// `c += 1;` where `c: char` — fix #5054 (§3.5, v0.307): arithmetic on `char`
// is not defined; `c += 1` desugars to `c := c + 1` (§6.6), and the `+`
// on `char` operands is rejected by reportBoolOperandExcludedFromArithmeticOrOrdering
// as extended by the fix. Previously the compiler accepted this through the
// char→int widening path.
//
// REJECT: must fail with the char-arithmetic diagnostic.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public static method Main(): int var c: char := 'a'; c += 1; return int(c); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `+` is not defined for `char` operands (§3.5)
