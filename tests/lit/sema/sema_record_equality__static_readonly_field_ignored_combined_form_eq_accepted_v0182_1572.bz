// Ported from tests/sema/record_equality_eligibility_test.cpp
//   (BEZEL_TEST sema_record_equality, static_readonly_field_ignored_combined_form_eq_accepted_v0182_1572).
//
// §9.2 v0.182 (via §9.3's combined-form application of the same rule): a
// static field is ignored on a COMBINED-form (positional + body) record too.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Vec2(X: int, Y: int) public static readonly Label: string := "vec2"; end Vec2; class C public method M() var a := new Vec2(1, 2); var b := new Vec2(1, 2); _ := a = b; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
