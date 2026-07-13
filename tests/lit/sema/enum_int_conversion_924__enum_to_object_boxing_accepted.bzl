// Ported from tests/sema/enum_int_conversion_924_test.cpp
//   (BEZEL_TEST enum_int_conversion_924, enum_to_object_boxing_accepted).
//
// §11.x: enum → Object boxing remains accepted (Object-target early-return).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum E A, B, C; end E; class C public static method Main(): int var e: E := E.B; var o: Object := e; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
