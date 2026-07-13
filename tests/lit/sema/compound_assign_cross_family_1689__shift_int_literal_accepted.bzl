// Ported from tests/sema/compound_assign_cross_family_1689_test.cpp
//   (BEZEL_TEST compound_assign_cross_family_1689, shift_int_literal_accepted).
//
// `s <<= 2;` — a shift's result is always the left operand's type (§6.5),
// so there is never a write-back mismatch to reject.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var s: short := 1; s <<= 2; return int(s); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
