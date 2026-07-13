// Ported from tests/sema/generic_method_constraint_callsite_746_test.cpp
//   (BEZEL_TEST method_constraint_callsite_746, constrained_t_iface_member_accepted_1078).
//
// §14.2 (#1078): ANTI-OVER-REJECT — a CONSTRAINED T (`where T: IFoo`) method
// call on member F() provided by IFoo must still be ACCEPTED (no error).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface IFoo method F(): int; end IFoo; class Holder<T> where T: IFoo private _item: T?; public method GetF(): int return _item!.F(); end GetF; end Holder; class C public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
