// Ported from tests/sema/hiding_override_test.cpp
//   (BEZEL_TEST sema_hide, override_of_virtual_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Base public virtual method Speak() _ := 1; end Speak; end Base; class Derived : Base public override method Speak() _ := 2; end Speak; end Derived;

// NODIAG-NOT: error
// NODIAG-NOT: §
