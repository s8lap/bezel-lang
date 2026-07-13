// Ported from tests/sema/enum_cast_const_range_859_test.cpp
//   (BEZEL_TEST enum_cast_arg_type_1171, enum_cast_accepts_int_arg).
//
// §5.4/§5.1: an integer-typed argument (the underlying type) is accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum Color Red, Green, Blue; end Color; class C public static method Main(): int var n: int := 1; var c: Color := Color(n); return int(c); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
