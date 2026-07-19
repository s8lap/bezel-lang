// Ported from tests/sema/function_type_assignability_1544_test.cpp
//   (BEZEL_TEST function_type_assignability_1544, covariant_return_nullable_target_accepted_spec_27_1_1).
//
// §27.1.1 second bullet + §7: nullable widening composes with covariant
// return — a non-null () -> Dog value is usable as a nullable
// (() -> Animal)? target.
//
// ACCEPT control: bezelc -c must exit zero. v0.201 (#2048): the fresh-producing
// factory is spelled `owned Dog` per the declaration-site static-return rule;
// the function-typed slot carries the marker through §27.1.1's exact-modifier
// matching.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NO-ERROR --allow-empty %s

module M; class Animal end Animal; class Dog: Animal end Dog; class C public static method MakeDog(): owned Dog return new Dog(); end MakeDog; public static method Main(): int var f: () -> owned Dog := MakeDog; var g: (() -> owned Animal)? := f; return 0; end Main; end C;

// NO-ERROR-NOT: error:
