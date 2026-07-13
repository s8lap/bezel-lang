// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/compound_assign_cross_family_1689_test.cpp
//   (BEZEL_TEST compound_assign_cross_family_1689, decimal_plus_double_rejected).
//
// §6.2/§6.6: `decimal` and `double` have no implicit conversion in either
// direction (§6.2's own example), so `d += 5.0` on `d: decimal` is the same
// family-crossing case. Before the fix this was silently ACCEPTED (a
// wrong-accept, not a crash): the double literal's LLVM value was fed
// straight to the decimal-arithmetic helper expecting the 4xi32 decimal
// struct.

module M; class C public static method Main(): int var d: decimal := 1.0m; d += 5.0; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `+=` has no valid result: `decimal + double` computes `double`, which is neither `decimal` nor implicitly convertible to it, and `decimal` is not an integer/char target receiving an integer result (the only case §6.6's write-back narrows) — convert the right-hand operand explicitly, or write `x := decimal(x + e);` if the loss is intended
// CHECK-SAME: (§6.6)
