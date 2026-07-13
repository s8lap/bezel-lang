// Ported from tests/ownership/discard_named_owned_binding_1651_test.cpp
//   (BEZEL_TEST discard_named_owned_binding_1651, owned_parameter_discard_no_autodrop_warning).
//
// ===========================================================================
// FAIL-WITHOUT / PASS-WITH #4: the `owned` PARAMETER form. Before the fix,
// the special-case branch this generalizes gated on `sema::LocalSymbol` —
// a ParameterSymbol never matched the dynamic_cast, so an `owned` parameter
// discard was unhandled exactly like the plain-local case: `r` stayed Owned
// to method end and drew the "owned parameter ... implicitly disposed at the
// end of the method" auto-drop must-use warning despite the explicit
// discard.
// ===========================================================================
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class R public _h: int := 0; destructor _h := 1; end destructor; end R; class C public static method Consume(owned r: R): int _ := r; return 0; end Consume; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
