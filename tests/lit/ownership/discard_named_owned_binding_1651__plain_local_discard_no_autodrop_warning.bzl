// Ported from tests/ownership/discard_named_owned_binding_1651_test.cpp
//   (BEZEL_TEST discard_named_owned_binding_1651, plain_local_discard_no_autodrop_warning).
//
// ===========================================================================
// FAIL-WITHOUT / PASS-WITH #1 (the headline #1651 defect): discarding a
// plain owned LOCAL with nothing left to do afterward must NOT draw the
// affine auto-drop must-use warning at scope end. Before the fix `x` stayed
// Owned through the discard (a no-op read), reached scope end still Owned,
// and the auto-drop pass fired its must-use warning — which even recommends
// `_ := x;` as the remedy, though that is exactly what the program already
// wrote (the literal symptom reported in #1651).
// ===========================================================================
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class R public _h: int := 0; destructor _h := 1; end destructor; end R; class C public static method Main(): int var x := new R(); _ := x; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
