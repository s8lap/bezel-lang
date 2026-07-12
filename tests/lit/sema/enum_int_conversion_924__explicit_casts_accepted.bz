// Ported from tests/sema/enum_int_conversion_924_test.cpp
//   (BEZEL_TEST enum_int_conversion_924, explicit_casts_accepted).
//
// §5.4: the explicit casts `int(e)` and `E(n)` remain accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum E A, B, C; end E; class C public static method Main(): int var e: E := E.C; var n: int := int(e); var e2: E := E(2); return n + int(e2); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
