// Ported from tests/sema/with_expression_target_validation_875_test.cpp
//   (BEZEL_TEST with_expression_target_validation_875, valid_positional_field_accepted).
//
// §9.5, #875-878 positive: valid positional-field replacement still compiles.
// `p with X := 5 end` where X: int and 5: int is accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Point(X: int, Y: int); class C public static method Main(): int var p := new Point(1, 2); var q := p with(X := 5); return q.X; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
