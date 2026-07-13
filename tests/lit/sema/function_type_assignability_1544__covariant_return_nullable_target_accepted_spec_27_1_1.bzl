// Ported from tests/sema/function_type_assignability_1544_test.cpp
//   (BEZEL_TEST function_type_assignability_1544, covariant_return_nullable_target_accepted_spec_27_1_1).
//
// §27.1.1 second bullet + §7: nullable widening composes with covariant
// return — a non-null () -> Dog value is usable as a nullable
// (() -> Animal)? target.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Animal end Animal; class Dog: Animal end Dog; class C public static method MakeDog(): Dog return new Dog(); end MakeDog; public static method Main(): int var f: () -> Dog := MakeDog; var g: (() -> Animal)? := f; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: static factory method `MakeDog` returns `Dog` with no `owned` qualifier and no structural borrow source; the caller treats the result as owned
// CHECK-SAME: (§28.3 sourceless-return rule)
