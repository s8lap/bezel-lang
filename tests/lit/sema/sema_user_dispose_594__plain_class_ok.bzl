// Ported from tests/sema/user_dispose_method_594_test.cpp
//   (BEZEL_TEST sema_user_dispose_594, plain_class_ok).
//
// A class with no Dispose or destructor at all — trivially fine.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Counter public method Increment() _ := 0; end Increment; end Counter;

// NODIAG-NOT: error
// NODIAG-NOT: §
