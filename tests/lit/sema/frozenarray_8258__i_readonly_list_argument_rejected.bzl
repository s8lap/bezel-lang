// fix #8258 (spec §5.1 *Implicit conversions*, stdlib §4.19, v0.355):
// an ANNOTATED `FrozenArray<int>` must NOT be admitted at an
// `IReadOnlyList<int>` parameter.
//
// stdlib §4.19 (v0.355) declares `public class FrozenArray<T>: IEnumerable<T>`
// — its ONLY interface; it declares no `Count` member. Spec §5.1 (v0.355)
// lists exactly one class-to-interface reference conversion — "a type to an
// interface it implements or conforms to: classes by declaration or
// inheritance" — so a `FrozenArray<T>` converts to `IEnumerable<T>` and to
// NOTHING else.
//
// Fails-without-fix: `ExprTC::variantGenericAssignable`'s builtin-hierarchy
// candidate search found no `IReadOnlyList` ancestor of `FrozenArray` and fell
// through to its top-level conservative ACCEPT, because `FrozenArray` was
// absent from `builtinBorrowingCollectionFullyModeled` — the predicate that
// makes a not-found candidate PROOF of non-conformance. The call compiled and
// printed `3` (the issue's repro).
//
// The admitting twin (`FrozenArray<int>` -> `IEnumerable<int>`, which stdlib
// §4.19 DOES declare) is the companion accept case
// `frozenarray_8258__ienumerable_argument_accepted.bzl`.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M;

class C
  static method N(xs: IReadOnlyList<int>): int => xs.Count;
  public static method Main(): int
    using var a: Array<int> := [1, 2, 3];
    using var g: FrozenArray<int> := FrozenArray<int>.From(a);
    Console.WriteLine(N(g));
    return 0;
  end Main;
end C;

// The §5.1 conversion diagnostic, with the spec section cited.
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the argument `xs` to call to `N` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
