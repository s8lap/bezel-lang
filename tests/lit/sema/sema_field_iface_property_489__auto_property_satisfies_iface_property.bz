// Ported from tests/sema/field_iface_property_489_test.cpp
//   (BEZEL_TEST sema_field_iface_property_489, auto_property_satisfies_iface_property).
//
// §11.2 / §23 — test 4 (CONTROL): a class with a matching AUTO-PROPERTY
// `public property Val: int` DOES satisfy the interface contract → no error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface IFoo property Val: int; end IFoo; class Box : IFoo public property Val: int := 0; public constructor(v: int) Val := v; end constructor; destructor end destructor; end Box;

// NODIAG-NOT: error
// NODIAG-NOT: §
