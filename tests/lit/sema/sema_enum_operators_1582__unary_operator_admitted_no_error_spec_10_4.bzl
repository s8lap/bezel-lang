// Ported from tests/sema/enum_operators_1582_test.cpp
//   (BEZEL_TEST sema_enum_operators_1582, unary_operator_admitted_no_error_spec_10_4).
//
// §10.4/A.3: a unary `operator -` inside an enum body is admitted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum Sign: int Neg := -1, Zero := 0, Pos := 1; public static method operator -(a: Sign): Sign => Sign(-int(a)); end Sign; class C public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
