// Ported from tests/sema/function_type_assignability_1544_test.cpp
//   (BEZEL_TEST function_type_assignability_1544, contravariant_param_widening_accepted_spec_27_1_1).
//
// §27.1.1 first bullet, spec example: "a (Animal) -> () value is usable
// where (Dog) -> () is expected" — the target's (narrower) param type
// reference-converts to the value's (wider) param type.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Animal end Animal; class Dog: Animal end Dog; class C public static method Main(): int var f: (Animal) -> () := (a: Animal) => do end; var g: (Dog) -> () := f; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
