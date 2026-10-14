// Ported from tests/sema/record_equality_eligibility_test.cpp
//   (BEZEL_TEST sema_record_equality, override_equals_plus_operator_eq_accepted_1249).
//
// §9.2 v0.156 / #1249: a body record with BOTH `override Equals`/`GetHashCode`
// AND explicit `operator =`/`<>` is valid — the author opted in (§9.2 "Authors
// who want explicit equality … declare `=` and `<>`").  `=` compiles.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record R public property V: int; public constructor(v: int) V := v; end constructor; public override method Equals(other: R): bool => true; public override method GetHashCode(): int => 0; public static method operator =(a: R, b: R): bool => a.V = b.V; public static method operator <>(a: R, b: R): bool => a.V <> b.V; end R; class C public method M() var a := new R(1); var b := new R(9); _ := a = b; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
