// Ported from tests/sema/record_equality_eligibility_test.cpp
//   (BEZEL_TEST sema_record_equality, ineligible_user_operator_eq_accepted_spec_8_4).
//
// §9.2 / §8.4: an ineligible body record that declares its own `=` / `<>`
// operator overloads is excluded from the error — `=` dispatches to the
// user-declared operator.  Per §25.2 v0.158 the homogeneous `=`/`<>` overload
// requires a co-declared `Equals(T)` + `override GetHashCode`.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Tagged public property X: int; private _cache: int := 0; public constructor(x: int) X := x; end constructor; public static method operator =(a: Tagged, b: Tagged): bool => true; public static method operator <>(a: Tagged, b: Tagged): bool => false; public method Equals(other: Tagged): bool => true; public override method GetHashCode(): int => 0; end Tagged; class C public method M() var a := new Tagged(1); var b := new Tagged(2); _ := a = b; _ := a <> b; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
