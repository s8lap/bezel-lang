// Ported from tests/sema/decimal_diagnostics_521_test.cpp
//   (BEZEL_TEST sema_521_static_field, decimal_in_method_accepted).
//
// §4.4: decimal arithmetic in an instance method → accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method Area(w: decimal, h: decimal): decimal return w * h; end Area; public static method Main(): int => 0; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
