// Ported from tests/sema/enum_cast_const_range_859_test.cpp
//   (BEZEL_TEST enum_cast_const_range_859, in_range_const_accepted).
//
// Accepted: an in-range constant cast is not flagged.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum Color: byte Red := 0, Green := 1, Blue := 2; end Color; class C public static method Main(): int var c: Color := Color(2); return int(c); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
