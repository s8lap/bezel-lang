// Ported from tests/sema/mixed_numeric_test.cpp
//   (BEZEL_TEST sema_mixed_numeric, int_plus_int_ok).
//
// Same-type operands are never a mixed-type case.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M(a: int, b: int): int var r := a + b; return 0; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
