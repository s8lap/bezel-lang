// Ported from tests/sema/override_generic_constraint_inheritance_1579_test.cpp
//   (BEZEL_TEST override_generic_constraint_inheritance_1579, reordered_clause_ok).
//
// §14.2: restating the same TWO-item constraint set in REVERSED order
// (`class, IFoo` vs the base's `IFoo, class`) is the SAME set, compared
// order-insensitively — compiles.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface IFoo end IFoo; class Base public virtual method Store<T>(x: T) where T: IFoo, class end Store; end Base; class Derived: Base public override method Store<T>(x: T) where T: class, IFoo end Store; end Derived;

// NODIAG-NOT: error
// NODIAG-NOT: §
