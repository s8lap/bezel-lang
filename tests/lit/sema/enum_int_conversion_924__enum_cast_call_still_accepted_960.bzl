// Ported from tests/sema/enum_int_conversion_924_test.cpp
//   (BEZEL_TEST enum_int_conversion_924, enum_cast_call_still_accepted_960).
//
// §5.4 (#960): the cast `E(n)` (a CallExpr, not a NewExpr) remains accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum E A, B, C; end E; class P public static method Main(): int var e: E := E(1); return int(e); end Main; end P;

// NODIAG-NOT: error
// NODIAG-NOT: §
