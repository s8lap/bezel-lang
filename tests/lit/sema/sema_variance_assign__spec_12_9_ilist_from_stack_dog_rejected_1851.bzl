// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §12.9 / §5.1 (issue #1851): `Stack<T>` conforms only to `IReadOnlyCollection<T>`
// → `IEnumerable<T>` (stdlib §4.11, v0.165 #1354); it does NOT implement the
// unrelated sibling `IList<T>` (borrowing mutable list, stdlib §4.1). Assigning
// `Stack<Dog>` to `IList<Dog>` is therefore not a reference conversion and must be
// rejected (§5.1) — accepting it would dispatch `IList` mutators/indexer through
// an itable the runtime object does not carry.
//
// Fails-without-fix: variantGenericAssignable's both-builtin candidate search
// found no `IList` ancestor of `Stack` and fell through to a top-level
// conservative-accept (`return true`), so the assignment compiled clean (rc=0).
// The fix rejects when `from` is a fully-modeled borrowing collection.

module M; class Animal end Animal; class Dog: Animal end Dog; class C public method M(a: Stack<Dog>) var x: IList<Dog> := a; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `x` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
