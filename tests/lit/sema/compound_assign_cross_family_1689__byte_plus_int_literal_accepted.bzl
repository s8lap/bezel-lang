// Ported from tests/sema/compound_assign_cross_family_1689_test.cpp
//   (BEZEL_TEST compound_assign_cross_family_1689, byte_plus_int_literal_accepted).
//
// `b += 4;` — R = int (byte widens to int, §6.2); byte is an integer
// target receiving an integer result, so this is the checked-narrowing
// write-back, not a compile error (`byte(264)` raises OverflowException at
// RUNTIME on an out-of-range sum, never here).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var b: byte := 250; b += 4; return int(b); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
