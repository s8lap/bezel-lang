// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #3879 (§22.12, v0.294): the inferred-type mirror of the explicit-form
// gap (`explicit_nullable_int_rejected`) — a class-level `const` with NO
// written type annotation is classified against the initializer expression's
// type (fix #3856's `ExprTypecheck.cpp` `ConstDecl` arm). That arm
// independently spelled the same `kind == Primitive && primitive != Object`
// test and likewise never consulted `nullable`, so
// `public const B := (if true then 1 else null);` — whose conditional-expr
// initializer infers to `int?` (the `then` branch's `int` widened to admit
// the `else` branch's `null`) — compiled silently. Both this and the
// explicit-form test are fixed by the ONE shared predicate
// (`isAdmissibleClassConstType`, ConstFold.hpp) so the two admission points
// cannot drift on this rule again.

module M; class C public const B := (if true then 1 else null); end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: const `B` must have a primitive value type or `string`
// CHECK-SAME: (§22.12)
