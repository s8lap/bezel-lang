// Ported from tests/sema/decimal_diagnostics_521_test.cpp
//   (BEZEL_TEST sema_521_static_field, instance_decimal_field_m_literal_accepted).
//
// §4.4: an instance decimal field with an m-literal → accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public _balance: decimal := 0m; public static method Main(): int => 0; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
