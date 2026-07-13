// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/override_generic_constraint_inheritance_1579_test.cpp
//   (BEZEL_TEST override_generic_constraint_inheritance_1579, narrowed_constraint_rejected).
//
// §14.2: the override REMOVES the base's constraint entirely (bare `where
// T: notnull` narrowed away from `IFoo`, a widened admission relative to the
// dispatch surface's checked constraint) — rejected.

module M; interface IFoo end IFoo; class Base public virtual method Store<T>(x: T) where T: IFoo end Store; end Base; class Derived: Base public override method Store<T>(x: T) where T: notnull end Store; end Derived;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: override of `Store` restates a `where T: ...` clause that differs from the inherited constraint set — omit the clause to inherit it exactly, or restate the identical set
// CHECK-SAME: (§14.2)
