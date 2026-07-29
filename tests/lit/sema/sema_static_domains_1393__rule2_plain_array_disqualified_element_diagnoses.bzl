// §22.12 rule 2 (v0.221 ruling / #1393, spec v0.228): a mutable static
// FIELD of plain (non-`owned`) inline-array type whose element type is
// OUTSIDE the form-(iii) primitive/enum recursion (`string`, here) is a
// compile error — "a `string`- or record-element array imports its
// element category's dilemma component-wise." This is distinct from the
// companion rule2_form3_mutable_static_plain_array_field_warns.bzl, whose
// `[3]int` element IS form-(iii)-eligible and only WARNS.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class App public static _labels: [3]string := ["a", "b", "c"]; public static method Main(): int => 0; end App;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: mutable static field `_labels` has type `[3]string`, which §22.12 rule 2 does not admit as a mutable static — a plain inline array's element type is outside the primitive/enum recursion (a `string`- or record-element array imports its element category's dilemma; a class- or interface-element array carries borrows, whose cross-domain dangling is memory-unsafe)
// CHECK-SAME: (§22.12 rule 2, v0.221, #1393)
