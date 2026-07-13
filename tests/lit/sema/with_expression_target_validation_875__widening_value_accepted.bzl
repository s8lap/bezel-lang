// Ported from tests/sema/with_expression_target_validation_875_test.cpp
//   (BEZEL_TEST with_expression_target_validation_875, widening_value_accepted).
//
// §9.5/§5.1, #875-878 positive: int → long widening is accepted (mirrors
// ordinary variable-declaration widening).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Big(N: long); class C public static method Main(): int var b := new Big(10); var c := b with N := 5 end; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
