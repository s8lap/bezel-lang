// §22.12 rule 2 table (v0.350): "owning inline array `[N owned]T?` /
// `[N owned]T` field" is `error (iii)-closed` when mutable -- the non-nullable
// element form `[N owned]T` too, initialized or not. Issue #7931.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class Cell public n: int := 0; end Cell; class App public static _pool: [2 owned]Cell := [new Cell(), new Cell()]; public static method Main(): int => 0; end App;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: mutable static field `_pool` has owning-inline-array type `[2 owned]Cell`, which §22.12 rule 2 does not admit as a mutable static
// CHECK-SAME: (§22.12 rule 2, v0.350, #1393)
