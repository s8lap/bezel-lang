// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #3879 (§22.12, v0.294): rule 2's verdict table states, for the
// "primitive value type … or enum type …" row, that the `const` column is
// "non-nullable primitive: ok; enum and both nullable forms: —". A nullable
// primitive `const` (`int?`) is therefore rejected — but checkTypeConsts
// (TypeResolution_Checks.cpp, the explicit-type-annotation admission point)
// tested only `kind == Primitive && primitive != Object`, never consulting
// `TypeRef::nullable`, so `public const A: int? := 1;` compiled silently.
// This is the explicit-type-annotation spelling of the gap; see the sibling
// `inferred_nullable_int` test for the inferred-type mirror (fix #3856) and
// `explicit_nullable_string` for the `string?` row.

module M; class C public const A: int? := 1; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: const `A` must have a primitive value type or `string`
// CHECK-SAME: (§22.12)
