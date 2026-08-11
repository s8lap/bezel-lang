// Ported from tests/sema/decimal_diagnostics_521_test.cpp
//   (BEZEL_TEST sema_521_m_literal, f_suffix_remains_float_spec_4_3).
//
// §4.3: `f` suffix → float, unchanged.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M(): int _ := 1.5f; return 0; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
