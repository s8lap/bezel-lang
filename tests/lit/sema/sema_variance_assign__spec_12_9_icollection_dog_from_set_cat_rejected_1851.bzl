// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §12.9 / §5.1 (issue #1851): `Set<T>` implements `ICollection<T>` (stdlib §4.6),
// which is INVARIANT in T (§12.9). `Set<Cat>` is thus assignable only to
// `ICollection<Cat>`, never to `ICollection<Dog>` (Cat and Dog are distinct
// invariant arguments), so the assignment is rejected (§5.1).
//
// Fails-without-fix: because the builtin hierarchy model under-reported
// `Set<T>` as `IReadOnlyCollection<T>` only (missing the `ICollection<T>` link),
// the both-builtin candidate search found no `ICollection` ancestor of `Set` and
// conservatively accepted the assignment — masking the invariant-argument
// mismatch. Completing the model to `Set : ICollection` both finds the candidate
// (enforcing invariance) and enables the sound reject.

module M; class Animal end Animal; class Dog: Animal end Dog; class Cat: Animal end Cat; class C public method M(a: Set<Cat>) var x: ICollection<Dog> := a; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `x` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
