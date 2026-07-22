// Ported from tests/sema/with_expression_target_validation_875_test.cpp
//   (BEZEL_TEST with_expression_target_validation_875, distinct_fields_in_with_accepted_935).
//
// ACCEPT: distinct field names on a record copy.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Point(X: int, Y: int); class C public static method Main(): int var p := new Point(1, 2); var p2 := p with(X := 5, Y := 9); return p2.X + p2.Y; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
