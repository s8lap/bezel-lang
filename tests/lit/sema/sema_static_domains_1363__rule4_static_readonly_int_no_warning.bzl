// Ported from tests/sema/static_field_domains_1363_test.cpp
//   (BEZEL_TEST sema_static_domains_1363, rule4_static_readonly_int_no_warning).
//
// (16) §22.12 rule 4 positive: static readonly int — no warning (value type,
// Rule 3 applies).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static readonly X: int := 5; public static method Main(): int => X; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
