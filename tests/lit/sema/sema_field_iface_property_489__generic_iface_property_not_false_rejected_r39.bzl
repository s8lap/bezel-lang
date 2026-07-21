// Ported from tests/sema/field_iface_property_489_test.cpp
//   (BEZEL_TEST sema_field_iface_property_489, generic_iface_property_not_false_rejected_r39).
//
// CONTROL: generic-interface conformance is NOT type-checked here (it needs
// type-argument substitution), so a generic interface implemented with the
// substituted concrete type must NOT be falsely rejected.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface IBox<T> readonly property Item: T; end IBox; class IntBox : IBox<int> public property Item: int => 7; end IntBox;

// NODIAG-NOT: error
// NODIAG-NOT: §
