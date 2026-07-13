// Ported from tests/sema/decimal_diagnostics_521_test.cpp
//   (BEZEL_TEST sema_521_m_literal, unsuffixed_float_remains_double_spec_4_3).
//
// §4.3 / §4.4: an unsuffixed float literal (1.5) remains double — not
// affected by the m-literal fix.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M(): int var _ := 1.5; return 0; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
