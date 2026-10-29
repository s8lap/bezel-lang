// §22.12 rule 2 (v0.221 ruling / #1393, spec v0.228): form (iv)'s
// owning-inline-array admission is also FIELDS only — a mutable static
// AUTO-PROPERTY of owning-inline-array type is a compile error regardless
// of `owned`-ness (the same closing-sentence rule as the plain-array
// auto-property case).
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class Cell public n: int := 0; end Cell; class App public static property Pool: [2 owned]Cell?; public static method Main(): int => 0; end App;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: mutable static property `Pool` has inline-array type `[2 owned]Cell?`; §22.12 rule 2 admits inline-array-typed mutable statics for FIELDS only (form (iii), plain heap-free element types) — an inline-array-typed auto-property is not admitted
// CHECK-SAME: (§22.12 rule 2, v0.221, #1393)
