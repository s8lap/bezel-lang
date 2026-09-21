// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Originally ported from tests/sema/compound_assign_cross_family_1689_test.cpp
//   (BEZEL_TEST compound_assign_cross_family_1689, decimal_plus_double_rejected).
//
// §5.1/§6.2/§6.6: `decimal` and `double` have no implicit conversion in
// either direction (§6.2's own example — on the §5.1 "never implicit"
// list), so `d += 5.0` on `d: decimal` is the same family-crossing case as
// the binary `decimal + double`. Before the ORIGINAL fix this was silently
// ACCEPTED (a wrong-accept, not a crash): the double literal's LLVM value
// was fed straight to the decimal-arithmetic helper expecting the 4xi32
// decimal struct.
//
// fix #4513 (§5.1/§6.2/§6.6, v0.306) changed the REJECTION WORDING (not the
// verdict — this stays a compile error either way). Since `decimal` and
// `double` have NO §5.1 relation in either direction, the desugared
// `decimal + double` is itself a §6.2 compile error — R never exists for
// this pair ("R the type of `x op e` under the rules of this chapter", §6.6
// — a pair with no result has no R to name) — so the correct diagnostic is
// the SAME pairwise-mismatch one the binary `decimal + double` already
// reports (fix #4501), not §6.6's write-back wording ("R is computed but
// does not convert back to T"), which presupposes a computed R that this
// pair never has. The write-back wording is reserved for a pair that DOES
// have a result — `byte += 1.5` (`byte` widens to `double`, R = `double`,
// which then fails to convert back — see the sibling
// `byte_plus_double_rejected` cell, unaffected by this fix).

module M; class C public static method Main(): int var d: decimal := 1.0m; d += 5.0; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `+` has no implicit conversion between operand types `decimal` and `double`
// CHECK-SAME: (§6.2); add an explicit cast (§5.2) to convert one operand to the other's type.
