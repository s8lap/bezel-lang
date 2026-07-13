// Ported from tests/sema/field_iface_property_489_test.cpp
//   (BEZEL_TEST sema_field_iface_property_489, block_property_satisfies_rw_iface_property).
//
// §11.2 / §23 — test 5 (CONTROL): block-form property with get+set satisfies
// a read-write interface property → no error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface ICounter property Count: int; end ICounter; class Box : ICounter protected _c: int := 0; public property Count: int get => _c; set => do _c := value; end; end Count; destructor end destructor; end Box;

// NODIAG-NOT: error
// NODIAG-NOT: §
