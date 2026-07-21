// Ported from tests/sema/field_iface_property_489_test.cpp
//   (BEZEL_TEST sema_field_iface_property_489, iface_property_type_match_ok_r39).
//
// CONTROL: a matching property type is accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface IVal readonly property Name: string; end IVal; class Impl : IVal public property Name: string => "hi"; end Impl;

// NODIAG-NOT: error
// NODIAG-NOT: §
