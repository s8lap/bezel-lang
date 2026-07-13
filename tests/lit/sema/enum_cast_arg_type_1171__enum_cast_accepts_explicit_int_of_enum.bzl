// Ported from tests/sema/enum_cast_const_range_859_test.cpp
//   (BEZEL_TEST enum_cast_arg_type_1171, enum_cast_accepts_explicit_int_of_enum).
//
// §5.4/§5.1: the explicit `int(e)` round-trip — convert an enum to its
// underlying integer, then cast that integer to another enum — is accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum A X, Y, Z; end A; enum B P, Q, R; end B; class C public static method Main(): int var a: A := A.Y; var b: B := B(int(a)); return int(b); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
