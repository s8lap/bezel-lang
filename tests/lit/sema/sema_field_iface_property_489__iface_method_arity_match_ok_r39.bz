// Ported from tests/sema/field_iface_property_489_test.cpp
//   (BEZEL_TEST sema_field_iface_property_489, iface_method_arity_match_ok_r39).
//
// CONTROL: a matching-arity implementation is accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface ICalc method Add(x: int, y: int): int; end ICalc; class Calc : ICalc public method Add(x: int, y: int): int return x + y; end Add; end Calc;

// NODIAG-NOT: error
// NODIAG-NOT: §
