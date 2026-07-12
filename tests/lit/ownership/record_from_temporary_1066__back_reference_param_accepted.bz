// Ported from tests/ownership/record_from_temporary_1066_test.cpp
//   (BEZEL_TEST record_from_temporary_1066, back_reference_param_accepted).
//
// A borrow parameter (a place with caller-frame storage) is the back-reference
// construction form. ACCEPT.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Bag(Items: Array<int>); record Pt(X: int, Y: int); class C public method Run(xs: Array<int>) var b := new Bag(xs); end Run; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
