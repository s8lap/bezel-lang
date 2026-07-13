// Ported from tests/sema/hiding_override_test.cpp
//   (BEZEL_TEST sema_hide, override_through_two_levels_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class A public virtual method M() _ := 1; end M; end A; class B : A public override method M() _ := 2; end M; end B; class C : B public override method M() _ := 3; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
