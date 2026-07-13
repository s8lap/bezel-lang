// Ported from tests/sema/bool_ordering_1574_test.cpp
//   (BEZEL_TEST sema_bool_ordering_1574, lt_on_char_still_admitted_control).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method M(a: char, b: char) _ := a < b; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
