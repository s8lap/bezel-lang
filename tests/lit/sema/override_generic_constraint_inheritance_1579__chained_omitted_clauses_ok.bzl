// Ported from tests/sema/override_generic_constraint_inheritance_1579_test.cpp
//   (BEZEL_TEST override_generic_constraint_inheritance_1579, chained_omitted_clauses_ok).
//
// §14.2: a THIRD-level override in a chain (Derived2 : Derived1 : Base) that
// also omits the clause inherits the same base constraint transitively — no
// error at any level.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface IFoo end IFoo; class Base public virtual method Store<T>(x: T) where T: IFoo end Store; end Base; class Mid: Base public override method Store<T>(x: T) end Store; end Mid; class Leaf: Mid public override method Store<T>(x: T) end Store; end Leaf;

// NODIAG-NOT: error
// NODIAG-NOT: §
