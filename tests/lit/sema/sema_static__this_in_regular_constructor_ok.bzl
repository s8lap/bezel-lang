// Ported from tests/sema/static_this_check_test.cpp
//   (BEZEL_TEST sema_static, this_in_regular_constructor_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public constructor() _ := this; end constructor; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
