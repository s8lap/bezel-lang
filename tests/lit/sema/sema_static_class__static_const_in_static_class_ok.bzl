// Ported from tests/sema/static_class_members_test.cpp
//   (BEZEL_TEST sema_static_class, static_const_in_static_class_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; static class Constants public const Pi: double := 3.14; end Constants;

// NODIAG-NOT: error
// NODIAG-NOT: §
