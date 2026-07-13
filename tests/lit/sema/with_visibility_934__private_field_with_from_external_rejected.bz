// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/with_visibility_934_test.cpp
//   (BEZEL_TEST with_visibility_934, private_field_with_from_external_rejected).
//
// §13.1, #934 — REJECT: `with` targets a private positional field from an
// external class. Positional fields are FieldSymbols; visibility still applies.

module M; record Pair private _first: int; private _second: int; public constructor(a: int, b: int) _first := a; _second := b; end constructor; end Pair; class C public static method Main(): int var p := new Pair(1, 2); var p2 := p with _first := 99 end; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `_first` is private to type `Pair` and not accessible from `C`
// CHECK-SAME: (§13.1 — `private` is visible to the declaring class only)
