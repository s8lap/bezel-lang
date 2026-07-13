// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_default_new_constraint_864_test.cpp
//   (BEZEL_TEST generic_default_new_constraint_864, part1_default_T_type_bound_rejected).
//
// §17.11 (#864 part 1): `default(T)` under a type-bound constraint (`where
// T: Foo`) also guarantees non-null T → same compile error.

module M; class Foo public constructor() end constructor; destructor end destructor; end Foo; static class Utils public static method GetNull<T>(): T? where T: Foo return default(T); end GetNull; end Utils; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `default(T)` is a compile error when T is constrained non-null (`where T: Foo`) — the null value `default(T)` does not fit T's non-null contract
// CHECK-SAME: (§17.11)
