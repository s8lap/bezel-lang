// Ported from tests/sema/hiding_override_test.cpp
//   (BEZEL_TEST sema_hide, interface_member_reimpl_is_not_hiding).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface IDoer method Do(); end IDoer; class C : IDoer public method Do() _ := 1; end Do; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
