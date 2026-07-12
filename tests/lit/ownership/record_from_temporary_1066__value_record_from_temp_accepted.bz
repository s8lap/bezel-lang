// Ported from tests/ownership/record_from_temporary_1066_test.cpp
//   (BEZEL_TEST record_from_temporary_1066, value_record_from_temp_accepted).
//
// A value-typed record (`Pt`, int fields) is not borrow-bearing — constructing it
// from literal args carries no borrow, so it is fine. ACCEPT.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Bag(Items: Array<int>); record Pt(X: int, Y: int); class C public method Run(xs: Array<int>) var p := new Pt(1, 2); end Run; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
