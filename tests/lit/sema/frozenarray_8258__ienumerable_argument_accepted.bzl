// fix #8258 (spec §5.1 *Implicit conversions*, stdlib §4.19, v0.355):
// the ADMITTING TWIN — `FrozenArray<int>` -> `IEnumerable<int>` must still be
// admitted.
//
// stdlib §4.19 (v0.355) declares `public class FrozenArray<T>: IEnumerable<T>`,
// so §5.1's "a type to an interface it implements or conforms to: classes by
// declaration or inheritance" DOES convert a `FrozenArray<T>` to
// `IEnumerable<T>`. The #8258 fix closes the over-acceptance at
// `IReadOnlyList<int>` (the companion reject case
// `frozenarray_8258__i_readonly_list_argument_rejected.bzl`) by adding
// `FrozenArray` to `builtinBorrowingCollectionFullyModeled` — it must NOT
// close the interface the class actually declares.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no error.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;

class C
  static method N(xs: IEnumerable<int>): int => 42;
  public static method Main(): int
    var a: Array<int> := [1, 2, 3];
    using var g: FrozenArray<int> := FrozenArray<int>.From(a);
    Console.WriteLine(N(g));
    return 0;
  end Main;
end C;

// No diagnostic of any kind — not even a warning.
// NODIAG-NOT: error
// NODIAG-NOT: §
