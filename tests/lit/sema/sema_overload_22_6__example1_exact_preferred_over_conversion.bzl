// Ported from tests/sema/overload_resolution_test.cpp
//   (BEZEL_TEST sema_overload_22_6, example1_exact_preferred_over_conversion).
//
// §22.6 Example 1 — exact match preferred over a conversion.
// The wrong overload Log(long) is placed first; §22.6 must still pick the
// exact Log(int) for an `int` argument. Log(int) returns int, Log(long)
// returns bool; binding to `var x: int` would error if Log(long) won.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Logger public method Log(level: long): bool => true; public method Log(level: int): int => level; public method Use(n: int) var x: int := this.Log(n); end Use; end Logger;

// NODIAG-NOT: error
// NODIAG-NOT: §
