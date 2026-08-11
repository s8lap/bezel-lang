// Ported from tests/sema/decimal_diagnostics_521_test.cpp
//   (BEZEL_TEST sema_521_m_literal, scientific_m_suffix_accepted_spec_4_4).
//
// §4.4: scientific notation decimal (1.23e2m) — accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M(): int _ := 1.23e2m; return 0; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
