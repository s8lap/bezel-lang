// Ported from tests/sema/enum_restrictions_test.cpp
//   (BEZEL_TEST sema_enum, enum_static_method_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum E A, B; public static method Count(): int => 2; end E;

// NODIAG-NOT: error
// NODIAG-NOT: §
