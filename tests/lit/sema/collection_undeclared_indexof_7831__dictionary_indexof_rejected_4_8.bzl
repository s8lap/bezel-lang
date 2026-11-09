// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #7831 (stdlib §1 Block-by-name, §4.8, v0.355): `Dictionary<K, V>`
// declares NO `IndexOf` either — stdlib §4.8's class block declares `Add`,
// `ContainsKey`, `Contains`, `Remove`, `TryGetValue`, `GetOrNull`,
// `GetOrAdd`, `Clear`, `IsReadOnly` and `GetEnumerator`.
//
// This row pins the DIAGNOSTIC, not the verdict: the call was already
// REJECTED before the #7831 fix, but with the generic #2457 backstop ("`IndexOf`
// on built-in type `Dictionary` produced no value, and no dispatch arm
// emitted any code for this call"). It now carries the stdlib's own
// documented-surface diagnostic — the same sentence the `IEnumerable` row of
// stdlib §4.12 uses, with the CONCRETE collection's phrasing ("this
// collection's documented surface"; the §4.12 `IEnumerable` row says "this
// interface's" because that head IS an interface).

module M;
import System;
class P
  public static method Main(): int
    var d := new Dictionary<int, int>();
    Console.WriteLine(d.IndexOf(1));
    return 0;
  end Main;
end P;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `IndexOf` on `Dictionary` (stdlib §4.8) -- it is not part of this collection's documented surface (Add/Clear/Contains/ContainsKey/Count/GetEnumerator/GetOrAdd/GetOrNull/IsReadOnly/Keys/Remove/TryAdd/TryGetValue/Values), a universal `ToString`/`Equals`/`GetHashCode`/`GetType` member (language spec §8.1), or a stdlib §5 LINQ extension operator
