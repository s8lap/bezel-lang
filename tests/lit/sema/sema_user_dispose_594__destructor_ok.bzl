// Ported from tests/sema/user_dispose_method_594_test.cpp
//   (BEZEL_TEST sema_user_dispose_594, destructor_ok).
//
// §28.4: a destructor (the correct customization point) is accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Resource destructor _ := 0; end destructor; end Resource;

// NODIAG-NOT: error
// NODIAG-NOT: §
