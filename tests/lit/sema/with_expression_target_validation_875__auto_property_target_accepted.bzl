// Ported from tests/sema/with_expression_target_validation_875_test.cpp
//   (BEZEL_TEST with_expression_target_validation_875, auto_property_target_accepted).
//
// §9.5, #875-878 positive: an auto-property with backing storage is a valid
// `with` target.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Person public Name: string; public Age: int; end Person; class C public static method Main(): int var p := new Person(); var q := p with(Age := 31); return q.Age; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
