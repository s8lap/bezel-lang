// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #7831 (stdlib §1 Block-by-name, §4.17, v0.355): `SortedDictionary<K,
// V>` declares NO `IndexOf`. Stdlib §4.17's class block for
// `SortedDictionary<K, V>` declares `Count`, the indexer `[k]`, `Add`,
// `TryAdd`, `ContainsKey`, `Contains`, `TryGetValue`, `GetOrNull`, `Remove`,
// `Clear`, `FirstKey` and `LastKey` — and no `IndexOf`; the name is declared
// only by `Array<T>` (stdlib §4.4) and `OwningArray<T>` (stdlib §4.5).
//
// Stdlib §1's Block-by-name rule (v0.350) makes an undeclared member an
// ERROR, and stdlib §5's `extension IEnumerable<T>` block declares no
// `IndexOf` either, so no extension can supply the name.
//
// BEFORE the fix this COMPILED (with only the #3079 "the argument of this
// `Console.WriteLine` failed to lower to a value" warning) and the program
// emitted NOTHING.
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
    var s := new SortedDictionary<int, int>();
    Console.WriteLine(s.IndexOf(1));
    return 0;
  end Main;
end P;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `IndexOf` on `SortedDictionary` (stdlib §4.17) -- it is not part of this collection's documented surface (Add/Clear/Contains/ContainsKey/Count/FirstKey/GetEnumerator/GetOrNull/LastKey/Remove/TryAdd/TryGetValue), a universal `ToString`/`Equals`/`GetHashCode`/`GetType` member (language spec §8.1), or a stdlib §5 LINQ extension operator
