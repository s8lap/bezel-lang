// Ported from tests/sema/decimal_diagnostics_521_test.cpp
//   (BEZEL_TEST sema_521_control, double_literal_no_suffix_unchanged_spec_4_3).
//
// §4.3: unsuffixed float literal is double — not affected by decimal fix.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M(): int var _ := 3.14; return 0; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
