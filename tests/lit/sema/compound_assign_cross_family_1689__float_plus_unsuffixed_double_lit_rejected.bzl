// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/compound_assign_cross_family_1689_test.cpp
//   (BEZEL_TEST compound_assign_cross_family_1689, float_plus_unsuffixed_double_lit_rejected).
//
// §6.6's own commented-out negative example: `f: float := 1.0f; f += 2.5;`
// — R = double (2.5 defaults to double, §4.3), which does not implicitly
// convert back to float.

module M; class C public static method Main(): int var f: float := 1.0f; f += 2.5; return int(f); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `+=` has no valid result: `float + double` computes `double`, which is neither `float` nor implicitly convertible to it, and `float` is not an integer/char target receiving an integer result (the only case §6.6's write-back narrows) — convert the right-hand operand explicitly, or write `x := float(x + e);` if the loss is intended
// CHECK-SAME: (§6.6)
