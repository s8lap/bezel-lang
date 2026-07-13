// Ported from tests/sema/record_equality_eligibility_test.cpp
//   (BEZEL_TEST sema_record_equality, eligible_eq_accepted_spec_9_2).
//
// §9.2: an eligible body record gets auto-equality — `=` is accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Pt public property X: int; public property Y: int; public constructor(x: int, y: int) X := x; Y := y; end constructor; end Pt; class C public method M() var a := new Pt(1, 2); var b := new Pt(1, 2); _ := a = b; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
