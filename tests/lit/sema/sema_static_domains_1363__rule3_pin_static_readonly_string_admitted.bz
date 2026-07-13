// Ported from tests/sema/static_field_domains_1363_test.cpp
//   (BEZEL_TEST sema_static_domains_1363, rule3_pin_static_readonly_string_admitted).
//
// (10) §22.12 rule 3 pin: static readonly of string — no error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static readonly Name: string := "bezel"; public static method Main(): int => 0; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
