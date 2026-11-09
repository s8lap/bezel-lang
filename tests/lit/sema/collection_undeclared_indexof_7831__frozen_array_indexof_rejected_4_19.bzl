// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #7831 (stdlib §1 Block-by-name, §4.19, v0.355): `FrozenArray<T>`
// declares NO `IndexOf`. Stdlib §4.19's class block for `FrozenArray<T>`
// declares the static `From` (two forms), `Length`, the indexer `[i]` and
// `ToArray` — and no `IndexOf`; the name is declared only by `Array<T>`
// (stdlib §4.4) and `OwningArray<T>` (stdlib §4.5).
//
// Stdlib §1's Block-by-name rule (v0.350) makes an undeclared member an
// ERROR, and stdlib §5's `extension IEnumerable<T>` block declares no
// `IndexOf` either, so no extension can supply the name.
//
// BEFORE the fix this COMPILED SILENTLY — `FrozenArray` is in NEITHER of
// sema's container tables (so no call-surface gate applies to it) and
// codegen's array `IndexOf` arm had no head gate, so the call took the
// `Array<T>` lowering.
//
// The rejection is the stdlib's own documented-surface diagnostic — the same
// sentence the `IEnumerable` row of stdlib §4.12 uses, with the CONCRETE
// collection's phrasing ("this collection's documented surface"; the §4.12
// `IEnumerable` row says "this interface's" because that head IS an
// interface).

module M;
import System;
class P
  public static method Main(): int
    const a: Array<int> := [1, 2, 1];
    using var f := FrozenArray.From(a);
    Console.WriteLine(f.IndexOf(1));
    return 0;
  end Main;
end P;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `IndexOf` on `FrozenArray` (stdlib §4.19) -- it is not part of this collection's documented surface (GetEnumerator/Length/ToArray), a universal `ToString`/`Equals`/`GetHashCode`/`GetType` member (language spec §8.1), or a stdlib §5 LINQ extension operator
