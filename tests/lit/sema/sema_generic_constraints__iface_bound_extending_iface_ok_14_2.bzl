// Ported from tests/sema/generic_constraints_test.cpp
//   (BEZEL_TEST sema_generic_constraints, iface_bound_extending_iface_ok_14_2).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface IFoo method F(); end IFoo; interface IBar : IFoo method G(); end IBar; class C<T> where T: IFoo private _x: T?; end C; class U private _u: C<IBar>?; end U;

// NODIAG-NOT: error
// NODIAG-NOT: §
