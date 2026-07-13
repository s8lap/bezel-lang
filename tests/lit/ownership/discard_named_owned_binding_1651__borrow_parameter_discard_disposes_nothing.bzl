// Ported from tests/ownership/discard_named_owned_binding_1651_test.cpp
//   (BEZEL_TEST discard_named_owned_binding_1651, borrow_parameter_discard_disposes_nothing).
//
// ===========================================================================
// CONTROL (unaffected by the fix either way): a discard whose operand is in
// BORROW state — here, a plain (non-`owned`) parameter — disposes nothing
// (v0.184, "borrow-state operands dispose nothing"); it is purely cosmetic,
// so a later read of the same borrow through the same binding remains valid.
// ===========================================================================
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class R public _h: int := 0; destructor _h := 1; end destructor; end R; class C public static method Touch(b: R): int _ := b; return b._h; end Touch; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
