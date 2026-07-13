// Ported from tests/sema/record_equality_eligibility_test.cpp
//   (BEZEL_TEST sema_record_equality, ineligible_tostring_call_accepted_spec_8_5).
//
// §8.5 / §9.2: `.ToString()` on an ineligible body record is NOT an error — the
// type-name default ToString applies. Only equality is gated.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record HasField public property X: int; private _cache: int := 0; public constructor(x: int) X := x; end constructor; end HasField; class C public method M() var a := new HasField(1); _ := a.ToString(); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
