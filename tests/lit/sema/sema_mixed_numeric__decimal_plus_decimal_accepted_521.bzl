// Ported from tests/sema/mixed_numeric_test.cpp
//   (BEZEL_TEST sema_mixed_numeric, decimal_plus_decimal_accepted_521).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M(a: decimal, b: decimal): int var r := a + b; return 0; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
