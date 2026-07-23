// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2163 (§6.5 / §3.5 / §7.11, spec v0.220): a `decimal?` TARGET of a compound
// bitwise op — the sharpest cell. §7.11 lifts an operator only where it is
// defined on the underlying type, and bitwise is not defined on decimal
// (§3.5), so there is nothing to lift. Before the fix `decimal?(1.5m) &= 3`
// compiled clean and the #2141 lifted-decimal lowering's `if (pay)` fallback
// silently stored the all-zero (NULL) struct — the statement NULLED the
// target (probe printed became-null, main @dd33abd).

module M; class C public static method Main(): int var d: decimal? := 1.5m; d &= 3; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `&=` is not valid on floating-point types — bitwise and shift operators require integer operands
// CHECK-SAME: (§6.5)
