// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2153 (§6.6 / §7.11 / §3.5, spec v0.220): `decimal? += bool?` — no operator
// `+` between decimal and bool. Before the fix this reached the #2141 decimal
// lowering with the bool payload reinterpreted through the decimal intrinsic:
// `decimal?(2.0m) += bool?(true)` silently stored the garbage value
// 1143708905468671979825532163 (probe, main @dd33abd).

module M; class C public static method Main(): int var d: decimal? := 2.0m; var b: bool? := true; d += b; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `+=` has no valid result: `decimal? + bool?` computes `bool?`, which is neither `decimal?` nor implicitly convertible to it
// CHECK-SAME: (§6.6)
