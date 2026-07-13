// Ported from tests/sema/override_generic_constraint_inheritance_1579_test.cpp
//   (BEZEL_TEST override_generic_constraint_inheritance_1579, omitted_clause_inherits_ok).
//
// §14.2: omitting the `where` clause on the override is the recommended
// spelling — inherits the base's constraint set exactly, no error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface IFoo end IFoo; class Base public virtual method Store<T>(x: T) where T: IFoo end Store; end Base; class Derived: Base public override method Store<T>(x: T) end Store; end Derived;

// NODIAG-NOT: error
// NODIAG-NOT: §
