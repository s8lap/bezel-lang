// Ported from tests/sema/record_equality_eligibility_test.cpp
//   (BEZEL_TEST sema_record_equality, ineligible_user_equals_accepted_spec_9_2).
//
// §9.2: an ineligible body record (explicit field) that declares its own
// `Equals` is excluded from the error — the call dispatches to the user member.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Tagged public property X: int; private _cache: int := 0; public constructor(x: int) X := x; end constructor; public method Equals(other: Tagged): bool => true; public method GetHashCode(): int => 0; end Tagged; class C public method M() var a := new Tagged(1); var b := new Tagged(2); _ := a.Equals(b); _ := a.GetHashCode(); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
