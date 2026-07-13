// Ported from tests/sema/override_generic_constraint_inheritance_1579_test.cpp
//   (BEZEL_TEST override_generic_constraint_inheritance_1579, alias_resolved_clause_ok).
//
// §14.2 / §27.8: restating the base's bound through a TYPE ALIAS of the same
// interface (`AliasOfIFoo` aliasing `IFoo`) is the same set after
// alias-resolution — compiles.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface IFoo end IFoo; type AliasOfIFoo := IFoo; class Base public virtual method Store<T>(x: T) where T: IFoo end Store; end Base; class Derived: Base public override method Store<T>(x: T) where T: AliasOfIFoo end Store; end Derived;

// NODIAG-NOT: error
// NODIAG-NOT: §
