// Ported from tests/sema/record_equality_eligibility_test.cpp
//   (BEZEL_TEST sema_record_equality, nontrivial_ctor_eq_accepted_v0182_1572).
//
// §9.2 v0.182 (#1572): "Removed in v0.182: the former … 'non-trivial
// constructor body' disqualifier[]. … no constructor body … can change what
// structural `=`, `GetHashCode`, or `ToString` mean." A constructor with
// control flow (`if … then … end`) — the exact #770 example that used to be
// rejected — no longer disqualifies: `=` compiles.
// (Regression guard, superseding the retired #770
// nontrivial_ctor_eq_diagnosed_770 test, which asserted the now-repealed
// behavior.)
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Clamped public property V: int; public constructor(v: int) V := v; if V > 100 then V := 100; end; end constructor; end Clamped; class C public method M() var a := new Clamped(5); var b := new Clamped(5); _ := a = b; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
