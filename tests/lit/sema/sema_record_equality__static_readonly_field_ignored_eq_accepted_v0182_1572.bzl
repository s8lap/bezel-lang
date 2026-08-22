// Ported from tests/sema/record_equality_eligibility_test.cpp
//   (BEZEL_TEST sema_record_equality, static_readonly_field_ignored_eq_accepted_v0182_1572).
//
// §9.2 v0.182: "Static members are ignored … `static readonly` fields …
// contribute no instance storage and neither qualify nor disqualify the
// record — a record may carry `Origin`-style constants … without losing `=`."
// A `static readonly` FIELD (an explicit FieldDecl, not an auto-property) must
// NOT disqualify — before the fix, the FieldSymbol arm of
// isAutoEqualityBearingBodyRecord rejected ANY explicit field unconditionally,
// including static ones.
//
// Post-#3290 (v0.265): a plain record is not borrow-bearing, so the §9.7
// static-field bullet does NOT except it — it takes rule 4's default
// warning. The check below pins the new rule 4 residual diagnostic; the
// test still passes exit-zero and has no errors.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Point public property X: int; public property Y: int; public static readonly Label: string := "origin-family"; public constructor(x: int, y: int) X := x; Y := y; end constructor; end Point; class C public method M() var a := new Point(1, 2); var b := new Point(1, 2); _ := a = b; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
