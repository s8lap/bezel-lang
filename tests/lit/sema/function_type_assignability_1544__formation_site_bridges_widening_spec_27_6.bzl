// Ported from tests/sema/function_type_assignability_1544_test.cpp
//   (BEZEL_TEST function_type_assignability_1544, formation_site_bridges_widening_spec_27_6).
//
// §27.1.1 fifth paragraph / §27.6: method-reference FORMATION at the
// assignment site is a DIFFERENT (broader) rule than value-assignability —
// "forming a reference from a named method against a target type may bridge
// any §5.1 implicit conversion, because the compiler synthesizes the adapter
// at the formation site." A direct `var g: (long) -> int := Twice;` (Twice's
// own natural type is (int) -> int) is therefore NOT rejected by the §27.1.1
// invariance rule that rejects the same shape mismatch once the value is
// already held in a variable (see primitive_param_invariant_rejected above).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Twice(x: int): int return x * 2; end Twice; public static method Main(): int var g: (long) -> int := Twice; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
