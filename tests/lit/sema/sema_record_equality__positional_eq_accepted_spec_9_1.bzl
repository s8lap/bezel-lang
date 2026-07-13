// Ported from tests/sema/record_equality_eligibility_test.cpp
//   (BEZEL_TEST sema_record_equality, positional_eq_accepted_spec_9_1).
//
// §9.1: a positional record gets field-by-field value equality — `=`,
// `.Equals`, and `.GetHashCode` are accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Point(x: int, y: int); class C public method M() var a := new Point(1, 2); var b := new Point(1, 2); _ := a = b; _ := a.Equals(b); _ := a.GetHashCode(); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
