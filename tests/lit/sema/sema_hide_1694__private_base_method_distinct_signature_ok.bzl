// Ported from tests/sema/hiding_override_test.cpp
//   (BEZEL_TEST sema_hide_1694, private_base_method_distinct_signature_ok).
//
// §12.2 (v0.184): when the base member is invisible at the derived
// declaration site, "the derived declaration creates a distinct member: no
// override relationship and no shared overload family exist between the
// two" — so a derived member of the SAME name but a DIFFERENT signature
// from an invisible (private) base method is legal too (it does not need to
// resemble an overload of the base method at all).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Base private method Speak() _ := 1; end Speak; end Base; class Derived : Base public method Speak(a: string) _ := 2; end Speak; end Derived;

// NODIAG-NOT: error
// NODIAG-NOT: §
