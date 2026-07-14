// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §12.9 (v0.196): the covariant conversion `IEnumerable<Array<int>>` →
// `IEnumerable<Array<long>>` requires the element conversion `Array<int>` →
// `Array<long>`. `Array` is invariant AND `int`/`long` are unrelated value
// types, so the inner conversion is doubly illegal and the assignment is
// rejected (§5.1). Issue #1842 accepted it because the nested arguments were
// never inspected.
//
// Fails-without-fix: variantGenericAssignable's `typeArgSameBase` matched the
// two `Array`s on base name alone, ignoring `int` vs `long`.

module M; class C public method M(a: IEnumerable<Array<int>>) var x: IEnumerable<Array<long>> := a; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `x` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
