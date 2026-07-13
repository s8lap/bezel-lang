// Ported from tests/sema/record_equality_eligibility_test.cpp
//   (BEZEL_TEST sema_record_equality, plain_auto_eq_unaffected_1249).
//
// §9.2 v0.156 / #1249 regression: a plain auto-equality body record (only
// auto-properties and a trivial constructor) still gets auto-equality — `=`
// must NOT be rejected.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Pt public property X: int; public property Y: int; public constructor(x: int, y: int) X := x; Y := y; end constructor; end Pt; class C public method M() var a := new Pt(1, 2); var b := new Pt(1, 2); _ := a = b; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
