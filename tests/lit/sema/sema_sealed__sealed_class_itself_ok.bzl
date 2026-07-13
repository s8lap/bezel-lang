// Ported from tests/sema/sealed_class_test.cpp
//   (BEZEL_TEST sema_sealed, sealed_class_itself_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; sealed class C end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
