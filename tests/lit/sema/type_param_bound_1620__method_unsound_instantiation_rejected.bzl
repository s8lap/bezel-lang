// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/type_param_bound_1620_test.cpp
//   (BEZEL_TEST type_param_bound_1620, method_unsound_instantiation_rejected).
//
// §14.2: `AsAnimal<Cat, Animal>` — `Cat` is not assignable to `Animal`, so the
// `where T: U` (U := Animal) obligation is UNSATISFIED — rejected.

module M; class Animal end Animal; class Dog: Animal end Dog; class Cat end Cat; static class Utils public static method Ident<T, U>(x: T): int where T: U where U: Animal return 0; end Ident; end Utils; class C public static method Main(): int using var c := new Cat(); return Utils.Ident<Cat, Animal>(c); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `Cat` does not satisfy the constraint `where T: Animal`
// CHECK-SAME: (§14.2)
