// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §22.3 (v0.169) call-site marker, FRESH-PRODUCER half, in the for-loop
// ITERABLE position (issue #2278; the walk that reaches this position is
// #2109's `forOrigins`).
//
// §22.3: "A **fresh producer** — a `new` expression, a call returning
// `owned T`, an operator result, or a `move obj._field` — stays unmarked: no
// live binding is invalidated, so there is nothing to announce. ... writing it
// on a borrow, a fresh producer, or a non-`owned` parameter position is
// likewise an error." (Same rule from §28.3's move-semantics paragraph: "and
// none anywhere for a fresh producer".)
//
// `C.Make()` is declared `owned OwningArray<OwningTask<Cell>>` — i.e. exactly
// the "a call returning `owned T`" form — so the `owned` marker on it must be
// REJECTED, even though `Task.WhenEachOwned`'s parameter IS an owned sink
// (stdlib §6.7 v0.212 / proposal 0023 D7). The diagnostic's parenthetical
// enumerates the fresh-producer forms; it is a statement of what the marker is
// NOT written on, not a list of admissible arguments.
//
// Fails-without / passes-with: this file pins the rejection so a future reading
// of the diagnostic as "these four forms are admissible" cannot silently
// re-admit the marker here. Before #2109 the iterable expression was never
// walked by the ownership pass, so this program compiled clean and FileCheck
// found nothing to match. The complement — the marker's VALID half over a
// named owned local — is covered by
// tests/codegen/task_wheneach_2082_test.cpp (owned_named_local_marker_transfers).

module M;
class Cell
  public static n: int := 0;
  public constructor() Cell.n := Cell.n + 1; end constructor;
  destructor Cell.n := Cell.n - 1; end destructor;
end Cell;
class C
  public static method Make(): owned OwningArray<OwningTask<Cell>>
    return new OwningArray<OwningTask<Cell>>();
  end Make;
  public static async method Main(): Task<int>
    var count := 0;
    await for var tk in Task.WhenEachOwned(owned C.Make()) do
      using var x := await tk;
      count := count + 1;
    end;
    return count;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the `owned` call-site marker is not written on a fresh producer
// CHECK-SAME: (a `new` expression, a call returning `owned T`, an operator result, or `move obj.f`)
// CHECK-SAME: remove `owned` (§22.3, v0.169)
