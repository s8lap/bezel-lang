// Ported from tests/sema/record_equality_eligibility_test.cpp
//   (BEZEL_TEST sema_record_equality, nontrivial_ctor_equals_call_accepted_v0182_1572).
//
// §9.2 v0.182 (#1572): `.Equals(...)` on the same record is likewise accepted
// now that a non-trivial constructor body no longer disqualifies.
// (Regression guard, superseding the retired #770
// nontrivial_ctor_equals_call_diagnosed_770 test.)
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Clamped public property V: int; public constructor(v: int) V := v; if V > 100 then V := 100; end; end constructor; end Clamped; class C public method M() var a := new Clamped(5); var b := new Clamped(5); _ := a.Equals(b); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
