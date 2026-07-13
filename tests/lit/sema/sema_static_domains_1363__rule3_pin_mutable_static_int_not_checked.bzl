// Ported from tests/sema/static_field_domains_1363_test.cpp
//   (BEZEL_TEST sema_static_domains_1363, rule3_pin_mutable_static_int_not_checked).
//
// (12) §22.12 rule 3 pin: mutable static int — no error (primitive types are
// not checked by the current Rule 2 implementation; see #1363-followup).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static _count: int := 0; public static method Main(): int => _count; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
