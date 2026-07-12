// Ported from tests/sema/static_virtual_test.cpp
//   (BEZEL_TEST sema_static_virtual, static_method_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method F(): int => 1; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
