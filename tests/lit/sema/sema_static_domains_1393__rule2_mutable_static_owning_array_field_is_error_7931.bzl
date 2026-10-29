// §22.12 rule 2 table (v0.350): the row "owning inline array `[N owned]T?` /
// `[N owned]T` **field**" is `error (iii)-closed` in the mutable column, and
// note (h) says "a mutable one is rule 2's closing error" -- the one admitted
// static of owning-inline-array type is `static readonly`. The warned
// admission this file used to pin (form (iv), #1393, v0.221) is retired:
// form (iv) is now the bare generic parameter. Issue #7931.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class Cell public n: int := 0; end Cell; class App public static _pool: [2 owned]Cell?; public static method Main(): int => 0; end App;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: mutable static field `_pool` has owning-inline-array type `[2 owned]Cell?`, which §22.12 rule 2 does not admit as a mutable static (rule 2's closing sentence; note (h): the one admitted static of owning-inline-array type is `static readonly`
// CHECK-SAME: (§22.12 rule 2, v0.350, #1393)
