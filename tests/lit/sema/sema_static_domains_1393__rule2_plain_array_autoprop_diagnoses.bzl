// §22.12 rule 2 (v0.221 ruling / #1393, spec v0.228): "inline-array-typed
// auto-properties remain compile errors under this rule's closing sentence"
// — forms (iii)/(iv) admit inline-array-typed mutable statics for FIELDS
// only. A mutable static AUTO-PROPERTY of plain (non-`owned`) inline-array
// type is a compile error even though the element type (`int`) would be
// form-(iii)-admitted on a FIELD (see the companion
// rule2_form3_mutable_static_plain_array_field_warns.bzl, which WARNS for
// the field spelling of the very same array type).
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class App public static property Buf: [3]int := [1, 2, 3]; public static method Main(): int => 0; end App;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: mutable static property `Buf` has inline-array type `[3]int`; §22.12 rule 2 admits inline-array-typed mutable statics for FIELDS only (form (iii), plain heap-free element types) — an inline-array-typed auto-property is not admitted
// CHECK-SAME: (§22.12 rule 2, v0.221, #1393)
