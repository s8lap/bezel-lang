// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/override_generic_constraint_inheritance_1579_test.cpp
//   (BEZEL_TEST override_generic_constraint_inheritance_1579, added_constraint_rejected).
//
// §14.2: the override ADDS a constraint (`class`) the base did not declare —
// an added constraint would go unenforced at every virtual call — rejected.

module M; interface IFoo end IFoo; class Base public virtual method Store<T>(x: T) where T: IFoo end Store; end Base; class Derived: Base public override method Store<T>(x: T) where T: IFoo, class end Store; end Derived;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: override of `Store` restates a `where T: ...` clause that differs from the inherited constraint set — omit the clause to inherit it exactly, or restate the identical set
// CHECK-SAME: (§14.2)
