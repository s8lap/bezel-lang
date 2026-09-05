// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #3879 (§22.12, v0.294) REGRESSION GUARD: rule 2's table rejects an
// enum-typed `const` in EITHER nullable form ("enum and both nullable
// forms: —"). This was already rejected before this issue's fix — an enum
// is a `TypeRef::Kind::Named` (not `Kind::Primitive`), so the pre-existing
// `kind == Primitive` test already excluded it regardless of nullability —
// but the shared predicate this fix introduces
// (`isAdmissibleClassConstType`, ConstFold.hpp) must keep rejecting it too.
// Pinned here so a future change to the shared predicate cannot silently
// re-admit a nullable enum const.

module M; enum Priority Low, High; end Priority; class C public const D: Priority? := Priority.Low; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: const `D` must have a primitive value type or `string`
// CHECK-SAME: (§22.12)
