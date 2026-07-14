// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §12.9 (v0.196): "concrete generic types are always invariant", and "Position
// is composed across nested type constructors." The covariant conversion
// `Array<Array<Dog>>` → `IEnumerable<Array<Animal>>` requires the element
// conversion `Array<Dog>` → `Array<Animal>` in covariant position — but `Array`
// is invariant, so that inner conversion is illegal and the whole assignment is
// rejected (§5.1). Accepting it (issue #1842) aliases an `Array<Dog>` as an
// `Array<Animal>` and lets a `Cat` be written into a `Dog` array — heap type
// pollution.
//
// Fails-without-fix: the same-base short-circuit in variantGenericAssignable
// (`typeArgSameBase(Array<Dog>, Array<Animal>)`) ignored the nested arguments
// and treated the two `Array`s as an exact match, wrongly accepting.

module M; class Animal end Animal; class Dog: Animal end Dog; class C public method M(a: Array<Array<Dog>>) var x: IEnumerable<Array<Animal>> := a; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `x` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
