// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #3879 (§22.12, v0.294) REGRESSION GUARD: the inferred-type spelling of
// the enum-nullable regression guard in this file's `explicit_nullable_enum`
// sibling. `public const E := (if true then Priority.Low else null);`
// infers to `Priority?` and was already rejected before this issue's fix
// (enum is `Kind::Named`, excluded by `kind == Primitive` regardless of
// nullability) — pinned here against the shared predicate this fix
// introduces so a future change to it cannot silently re-admit this form.

module M; enum Priority Low, High; end Priority; class C public const E := (if true then Priority.Low else null); end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: const `E` must have a primitive value type or `string`
// CHECK-SAME: (§22.12)
