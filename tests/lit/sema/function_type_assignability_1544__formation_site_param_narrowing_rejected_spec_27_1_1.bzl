// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/function_type_assignability_1544_test.cpp
//   (BEZEL_TEST function_type_assignability_1544, formation_site_bridges_widening_spec_27_6).
//
// §27.1.1 fifth paragraph / §27.6: method-reference FORMATION at the
// assignment site is a DIFFERENT (broader) rule than value-assignability —
// "forming a reference from a named method against a target type may bridge
// any §5.1 implicit conversion, because the compiler synthesizes the adapter
// at the formation site." But the PARAMETER slot stays CONTRAVARIANT even at
// formation (§27.1.1 first bullet): the target's declared parameter type
// must convert TO the source's, not the reverse. `var g: (long) -> int :=
// Twice;` (Twice's own natural type is `(int) -> int`) needs `long`
// convertible to `int` — a NARROWING §5.1 never admits — so this is
// rejected, exactly as
// unbound_ext_ref_recv_param_check_4081.int_recv_long_param_narrowing_rejected_spec_27_6_27_1_1
// (issue #4081) already pins for an unbound extension-method reference.
//
// fix #4897 (§27.1.1 v0.307): this file previously asserted the OPPOSITE
// verdict (a clean-compile ACCEPT) under the name
// "...formation_site_bridges_widening_spec_27_6.bzl" — but a `(long) -> int`
// parameter against an `(int) -> int` source is a narrowing, not a
// widening, of the kind #4081 already established must be rejected. The
// accept verdict was never really exercising #4081's contravariance check:
// `Twice` here is a BARE (unqualified) method-group identifier, and bare
// method groups never received a Function TypeRef at all (checkAssignable's
// `from` arrived null, so the whole Function-to-Function block — #4081's
// check included — silently never ran; `int.Doubled4081b` in the #4081 test
// is instead an EXTENSION reference, unaffected by that gap, which is why
// only #4081's OWN gtest ever caught the rule). Fix #4897 adds the missing
// Function TypeRef synthesis for a bare method-group reference, which
// finally lets this exact case reach #4081's pre-existing, correct
// contravariant check — revealing that this file's long-standing verdict
// was wrong. Renamed and corrected rather than left silently masked; the
// genuine widening-bridge case (return-slot, not parameter-slot) is already
// covered by
// unbound_ext_ref_recv_param_check_4081.int_return_long_param_return_widening_accepted_spec_27_6_27_1_1.

module M; class C public static method Twice(x: int): int return x * 2; end Twice; public static method Main(): int var g: (long) -> int := Twice; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `g` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
