// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §12.9 (v0.196): the same nested-invariant hole through a USER covariant
// interface. `ICov<out T>` is covariant, so `ICov<Array<Dog>>` →
// `ICov<Array<Animal>>` requires `Array<Dog>` → `Array<Animal>` in covariant
// position — illegal because `Array` is invariant. Must be rejected (§5.1);
// issue #1842 accepted it.
//
// Fails-without-fix: the per-argument covariant check bottomed out at
// `typeArgSameBase(Array<Dog>, Array<Animal>)` = true, never recursing into the
// nested `Dog`/`Animal` arguments.

module M; class Animal end Animal; class Dog: Animal end Dog; interface ICov<out T> method Get(): T; end ICov; class C public method M(b: ICov<Array<Dog>>) var x: ICov<Array<Animal>> := b; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `x` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
