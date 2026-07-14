// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §12.9 (v0.196): the contravariant analogue of the nested-invariant hole.
// `ICon<in T>` is contravariant, so `ICon<Array<Animal>>` → `ICon<Array<Dog>>`
// requires (operands swapped) `Array<Dog>` → `Array<Animal>` in covariant
// position — illegal because `Array` is invariant. Must be rejected (§5.1);
// issue #1842 accepted it.
//
// Fails-without-fix: the contravariant branch used the same nested-blind
// `typeArgSameBase` short-circuit as the covariant one.

module M; class Animal end Animal; class Dog: Animal end Dog; interface ICon<in T> method Put(x: T): (); end ICon; class C public method M(s: ICon<Array<Animal>>) var x: ICon<Array<Dog>> := s; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `x` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
