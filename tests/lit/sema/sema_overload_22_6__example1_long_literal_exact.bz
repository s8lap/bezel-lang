// Ported from tests/sema/overload_resolution_test.cpp
//   (BEZEL_TEST sema_overload_22_6, example1_long_literal_exact).
//
// §22.6 Example 1 — a `long` literal (42L) is an exact match for Log(long).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Logger public method Log(level: int): int => level; public method Log(level: long): bool => true; public method Use() var x: bool := this.Log(42L); end Use; end Logger;

// NODIAG-NOT: error
// NODIAG-NOT: §
