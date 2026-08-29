# Bezel Guide 5: Collections, Queries, and Iteration

*A short, example-first guide. Companion to the Bezel Language Specification — re-validated against v0.284.*

The surface here looks familiar: resizable arrays, dictionaries, sets, stacks and queues (stdlib §4), and a query surface of `Where`/`Select`/`GroupBy`-style operators over any `IEnumerable<T>` (stdlib §5, auto-imported since v0.278 — no `import` needed). What is *not* familiar is how ownership runs through it, and that is what this guide teaches. Three rules change ingrained habits, each headed **How Bezel does it**. Every standard collection comes in **two families** — one that borrows its elements and one that owns them (§1). The loop variable of a `for` is not always yours to keep or dispose (§2, §3). And a **bound lazy query is single-consumption** — enumerate it twice and the compiler stops you (§4). Ownership basics (owned vs borrowed, `using var`, the `move` marker) are Guide 1's job; the `Try` shapes and the `*OrNull` tier are Guide 2 §2. If you skim everything else, read the three **How Bezel does it** sections and the table in §8.

**Contents:** §1 the two families · §2 what the loop variable is · §3 consuming iteration: `Drain()` and the take-forms · §4 affine queries · §5 `GroupBy` · §6 `First` at class elements · §7 spans and buffers · §8 choosing, and the five traps.

---

## 1. How Bezel does it: two collection families

Elsewhere, one collection type serves every purpose, because a garbage collector cleans up whatever the collection referenced. Bezel has no collector, so a collection must answer Guide 1's question for its elements: *is this container responsible for disposing them, or is someone else?* Each standard shape ships in two forms (spec §28.11; stdlib §4):

- **Borrowing collections** — `Array<T>`, `Dictionary<K, V>`, `Set<T>`, `Stack<T>`, `Queue<T>`, `LinkedList<T>`, and the sorted forms. Elements stay owned by whoever put them there. The collection's `Dispose()` does no semantic work — the borrowing forms are **trivial-dispose** (spec §28.9; Guide 1 §2): no destructor runs and no element is disposed, while the collection's own backing storage is part of the instance's single allocation, released with it when the compiler frees the collection at its owner's release point (spec §28.11; stdlib §4.4). `Remove` and `Clear` drop references and dispose nothing.
- **Owning collections** — `OwningArray<T>`, `OwningDictionary<K, V>`, `OwningSet<T>`, `OwningStack<T>`, `OwningQueue<T>`, `OwningLinkedList<T>`. `Add`/`Push`/`Enqueue` take `owned` arguments — ownership transfers in. `Remove`, `Clear`, and the indexer-set **dispose** the affected elements, and the collection's `Dispose()` cascades through every element it still holds.

```bezel
using var index := new Array<Connection>();      // borrowing
index.Add(primary);                              // primary stays owned by its owner
index.Clear();                                   // drops the reference; disposes nothing

using var pool := new OwningArray<Connection>(); // owning
pool.Add(Connect("primary"));                    // Connect returns owned — transfers in
pool.Add(Connect("backup"));
var c := pool[0];                                // indexer-get: a borrow of the slot
c.Ping();                                        // use it; you may not dispose it
// scope end: pool disposed → both connections disposed with it, no destructor written
```

The `Owning` prefix names the **element** axis, not the container axis: `owned OwningArray<Dog>` is an owning array *you* own, while a bare `OwningArray<Dog>` parameter is a borrow of someone else's owning array (stdlib §4). One classification drives every element operation — the **element slot policy** (spec §28.11). A slot's behavior follows from the family and the element's kind, never from per-collection quirks.

| Element type | Borrowing family | Owning family |
|---|---|---|
| value type (`int`, records) | copies in and out (a *borrow-bearing* record — §7, spec §9.7 — additionally requires every source it carries to outlive the collection) | *(rejected — nothing to own)* |
| `string`, function values | shared references | *(rejected)* |
| class / interface | slots hold **borrows** | slots are **owned** |

So an owning collection admits only class or interface elements (`where T: class` — a value has no disposal obligation to take). For `OwningDictionary<K, V>` the split is per-axis: values owned, keys value-like (stdlib §4.9). Reading is uniform across both families — the indexer-get and default iteration produce a borrow (or a copy, for value elements). Reading never transfers ownership.

**Picking one is stating intent.** A holder that must dispose its elements says so in the type — `private owned _sessions: OwningArray<Session>;` — and the cascade replaces any destructor (Guide 1 §5). A collection that merely *indexes* objects owned elsewhere is the borrowing form, and storing a borrow carries the usual obligation: the element's owner must outlive the collection's use of it (spec §28.11).

**The names tell you whether you now owe a disposal** (stdlib §1; spec §22.6). `Get`, `Peek`, `GetOrNull`, and the indexer are borrows: no obligation. On the **owning** family, `Pop`, `Dequeue`, `PopValue`, and `Drain` are transfers: you discharge the result. (The borrowing `Stack<T>`/`Queue<T>` at class elements hand back the same borrow they dropped — `Pop` and `Dequeue` there produce borrows sourced at the collection, stdlib §4.11.)

```bezel
// jobs: OwningStack<Job>
var top := jobs.Peek();          // borrow — the stack still owns it
using var next := jobs.Pop();    // transfer — yours now, disposed at scope end
```

**Ingress consumes — even when the collection declines the element.** An owning `Add` takes ownership at the call site, on every path. `OwningSet<T>.Add(owned item)` on a duplicate returns `()` and *disposes the passed-in item* — you relinquished it, and the set has no use for a second copy (stdlib §4.10). `OwningDictionary<K, V>.Add` on a present key throws, with the consumed value disposed on that path (stdlib §4.9). When you must keep the element on the declined path, ask first:

```bezel
if not seen.Contains(candidate) then
  seen.Add(move candidate);      // ownership transfers (call-site marker, Guide 1 §1)
end;
// on the duplicate path, candidate is still yours
```

(`TryAdd(owned item): bool` reports the outcome instead; the duplicate is still disposed.)

**Dictionaries in practice.** The lookup tiers are Guide 2 §2's: the indexer throws `KeyNotFoundException` (absence is a bug here), `GetOrNull` composes with `??`, `TryGetValue` distinguishes what a collapsed `T?` cannot (stdlib §4.8). At *class-typed* values `GetOrNull` returns a borrow of the stored value — a look, not a take. A borrow never meets an *owned* fallback through `??` (Guide 1 §8's meet rule): null-test instead, and use `PopValue`/`TryPopValue` when you mean take. Two adjacent habits are worth noting. `dict[key] += 1` updates but never inserts — it throws on an absent key — so tallying is `Counter<K>`'s job (`tally.Increment(word);`, stdlib §4.20). And `GetOrAdd(key, factory)` is a read path whose factory, on the owning map, produces an `owned V` the dictionary keeps — the call still returns a borrow.

Two conventions are worth absorbing in passing (stdlib §1). `Count` counts membership that grows and shrinks (`Array<T>.Count`); `Length` measures a fixed extent (buffers, spans, `FrozenArray<T>`, `string`). And the imperative form mutates in place (`Sort`, `Reverse`) while the participle returns a fresh owned copy (`Sorted()`, `Reversed()`) — *declared* on the borrowing family only, because an owning copy would double-own every element: `OwningArray<T>` declares no `Sorted`, no `Reversed`, no `ToArray` (stdlib §4.4). That is its **declared surface**, not what resolves on it. §5's `ToArray()`/`ToSet()`/`ToDictionary(…)` are extensions on `IEnumerable<T>`, which the owning family implements, so `pool.ToArray()` does resolve — and is sound, because what it returns is a **borrowing snapshot**: a fresh `Array<T>` whose slots borrow the elements `pool` still owns. No second owner is minted; the obligation is a lifetime one, `pool` must outlive every use of the snapshot, and `Clear`, `Remove`, `RemoveAt`, or an indexer-set kills the slot a snapshot entry points at (stdlib §4.5, v0.266). To reorder in place use `Reverse()`/`Sort()`; to move the elements into a new owning collection, `Drain()`.

**Reading code doesn't need to care which family.** The two families never interconvert: an `OwningArray<Connection>` *is not* an `Array<Connection>`. But both implement the read-only interfaces `IEnumerable<T>`, `IReadOnlyCollection<T>`, `IReadOnlyList<T>`, because reading produces borrows either way (spec §28.11; stdlib §4.3). So a consumer that only reads declares one of those and accepts either source:

```bezel
method ReportEach(items: IEnumerable<Connection>)
  for var c in items do Console.WriteLine(c.Name); end;
end ReportEach;

ReportEach(index);     // Array<Connection>: fine
ReportEach(pool);      // OwningArray<Connection>: fine — iteration borrows either way
```

The mutating interfaces are deliberately separate hierarchies (`IList<T>` vs `IOwningList<T>`): `Add` means something different in each, so code that abstracts over mutation picks one.

---

## 2. How Bezel does it: the loop variable is not always yours

`for var x in source do … end;` compiles to the enumeration protocol (`GetEnumerator()`/`MoveNext()`/`Current`, spec §29.1–§29.2), and **what `x` is follows from the source**:

| Source | `x` is |
|---|---|
| value elements (`Array<int>`, a range, a `string`'s chars) | a copy |
| `string` or function-typed elements | a shared reference |
| class elements of a collection — either family, default iteration | a **borrow** of the slot |
| an iterator's enumerator-owned yields — minted, or adopted into a frame binding (`IEnumerable<T>` at class T) | a borrow of the **enumerator's** slot |
| `coll.Drain()`, or any `IOwningEnumerable<T>` | **owned** — you discharge it (§3) |
| `Dictionary<K, V>` | a `DictionaryEntry<K, V>`; `entry.Value` borrows at class V |
| spans, inline arrays | direct indexed loop; copy or borrow per slot (§7) |

Two consequences do the habit-breaking.

**An element borrow does not escape the loop.** Over a *concrete* collection the borrow's source is the collection itself, so keeping a reference past the loop is fine while the collection lives — with §1's residual still yours on the borrowing family: the elements' own owners must outlive that use (spec §28.11). Over an *interface-typed* source the compiler cannot see the backing — the yielded value may live in the enumerator, which the loop disposes — so the variable's source is conservatively the loop's own enumerator, and any escape is rejected (spec §28.3/§29.2, v0.235):

```bezel
method Report(samples: IEnumerable<Sample>)
  var best: Sample? := null;
  for var s in samples do
    if best = null or s.Score > best!.Score then
      best := s;               // ERROR: through the interface, `s` borrows the
    end;                       //   loop's enumerator — disposed when the loop ends
  end;
end Report;
```

The remedy is to give the borrow a backing that outlives the loop. The direct form is a concrete collection type, where the compiler can see the chain slot → collection → binding:

```bezel
method Report(samples: Array<Sample>)   // concrete: the array is the backing
  var best: Sample? := null;
  for var s in samples do
    if best = null or s.Score > best!.Score then
      best := s;                 // fine: `s` borrows a slot of `samples`,
    end;                         //   which outlives the loop
  end;
  Console.WriteLine(best?.Name ?? "no samples");
end Report;
```

When you are handed an interface over a collection-backed sequence, materializing first (`.ToArray()`, §4) gives you such a concrete backing to loop over. When the source *mints* its elements (an iterator yielding `new …`), the elements need an owner that outlives the loop: the owning path of §3, or an owning collection built up front. (This is also why you cannot write your own `First()` over arbitrary `IEnumerable<T>` at class elements — see §6.)

**You never dispose a borrowed element.** Only owners dispose (spec §28.6; Guide 1 §9), and a default-iteration variable, an indexer-get, and `entry.Value` are all borrows:

```bezel
for var conn in pool do          // default iteration over an OwningArray: borrows
  conn.Dispose();                // ERROR: only owners dispose — no exception to this
end;
```

If the elements are the pool's to dispose, say so through the pool: `pool.Clear()` disposes every element, `pool.RemoveAt(i)` disposes one, and disposing the pool cascades. To dispose elements *yourself*, take them out first — that is §3.

Two smaller rules complete the picture. Structurally modifying a collection while iterating it raises `InvalidOperationException` at the next advance (spec §29.12). The standard shape is collect-then-remove, two passes:

```bezel
using var expired := new Array<Transaction>();
for var t in ledger do
  if t.IsExpired then expired.Add(t); end;   // collect borrows; mutate nothing yet
end;
for var t in expired do
  ledger.Remove(t);                          // structural changes outside the first loop
end;
```

(An element-level write that changes no structure — `arr[i] := v` on an existing position — does *not* invalidate the enumerator (spec §29.12). On an owning collection it disposes the displaced element, which invalidates borrows of *that element*, not the loop — spec §29.12.) And inline arrays (`[N]T`, spec §15.1) iterate as direct indexed loops with the same slot logic: copies for value slots, borrows for class slots — including `[N owned]T` slots, which the array keeps owning. Consuming one is an explicit `move arr[i]` plus restoration, and the owning form is a move-only value (spec §15.3).

---

## 3. Consuming iteration: `Drain()` and the take-forms

Default iteration looks; `Drain()` takes. Every owning collection has it: `Drain()` returns an `IOwningEnumerable<T>` (stdlib §4.13), the loop variable is **owned**, and each element leaves the collection as it is yielded (spec §28.11). The habit from Guide 1 §9: adopt on the body's first line, so every exit edge is covered.

```bezel
for var conn in pool.Drain() do
  using var c := move conn;      // adopt: yours for this iteration, disposed at body end
  c.SendShutdown();
end;
// pool is now empty
```

Skip the adoption and the element auto-drops at the body's end with a per-iteration must-use warning — a drop is never silent. Partial iteration drains only what was yielded: `break` out and the remaining elements are still in the collection, intact (spec §28.11). `OwningDictionary<K, V>` drains entries that own their values (stdlib §4.9):

```bezel
for var entry in sessions.Drain() do
  using var e := move entry;     // DrainedEntry owns its Value; adopting disposes both
  Archive(e.Key, e.Value);       // e.Value: the owned Session, borrowed by Archive
end;
```

(The *borrowing* `Dictionary<K, V>` iterates entries directly — `for var entry in dict` yields `DictionaryEntry<K, V>` values in insertion order, `entry.Value` a borrow at class V. The owning dictionary deliberately has **no** default entry iteration: its common intent is transfer, so you either iterate `Keys` to look or `Drain()` to take — stdlib §4.8/§4.9.)

For *one* element, use the take-forms: `PopAt(i)`/`PopFirst()`/`PopLast()` on `OwningArray<T>`, `Pop()`/`Dequeue()` on the owning stack and queue, `PopValue(key)` on the owning dictionary, `Pop(query)` on the owning set (named `Take` through v0.258 — the v0.259 rename un-shadows LINQ's `Take(count)` on set receivers, stdlib §4.10). Each has a `Try*` variant except `PopAt` (Guide 2 §2 shows the `Try` + `out using var` call shape). They remove the slot and hand you the owned element in one call. The low-level form is `move coll[i]`. It takes the element but leaves the slot **moved-from**, and the field-restoration rule applies: restore the slot before the method can exit (spec §28.8.1/§28.11; the same discipline as `move` on a field, Guide 1 §7):

```bezel
var fresh := Connect("fresh");   // build the replacement first — it may throw
using var old := move pool[i];   // then take — nothing throwable follows
pool[i] := fresh;                // restore the slot — required on every exit path
```

(The build-before-take order is Guide 1 §7's: a throwable call between the take and the restoration would put an exit edge there with the slot still moved-from, and the compiler rejects it.)

Prefer the `Pop*` forms unless you are replacing the slot in place — they close the gap and no restoration obligation arises.

**Writing your own iterators.** A method whose body uses `yield` is an iterator (spec §29.3); its body does not run at the call — it runs incrementally as the consumer advances (spec §29.4):

```bezel
public static method Tail(lines: Array<string>, n: int): IEnumerable<string>
  for var i in Math.Max(0, lines.Count - n)..<lines.Count do
    yield return lines[i];
  end;
end Tail;
```

Five rules cover the shape:

- An `IEnumerable<T>`-returning iterator is re-iterable — each `GetEnumerator()` runs the body afresh — while an `IEnumerator<T>` return is single-use (spec §29.3).
- An iterator's declared type is announcement enough at *every* callee kind — static, instance, or extension (type-announced; calls mint, spec §28.3, v0.258 — the full declared-kind × callee-kind matrix is §28.3's announcement table) — so the unmarked spelling above is the whole convention, and `owned IEnumerable<T>` is admissible emphasis. A *stored* collection you want to re-expose is not a method's job: expose it as a **property** (`public property Items: IEnumerable<Item> => _items;`), whose reads are borrows — a method returning the field no longer compiles.
- There is no plain `return` in an iterator — exit early with `yield break;` (spec §29.5).
- Resource handling inside the body prefers `using var` over `try`/`finally` (`yield` is barred inside a `try` that has a `catch`, spec §29.6). Cleanup is reliable on abandonment: if the consumer `break`s or the enumerator is disposed early, the frame runs its pending `using var` disposals and `finally` clauses exactly as if the body had exited at the suspension point (spec §29.7). An `owned` parameter the body never moved out is disposed by the frame too (spec §29.8; adopt-on-entry is the warning-free spelling, Guide 1 §9).
- What a yield *hands over* follows the return type: yields from an `IEnumerable<T>` iterator at class T are retained by the enumerator — the consumer borrows them (§2, §6) — while an `IOwningEnumerable<T>` iterator passes ownership through to the consumer, `Drain()`-style. Yielding a *named* owned local in a borrowing iterator is the adopt-then-yield idiom (Guide 1 §9).

---

## 4. How Bezel does it: a bound query is consumed exactly once

The stdlib §5 operators (auto-imported since v0.278 — the module is on spec §2.3's list, so no `import` is needed) are the headline unusual feature. As elsewhere, most of them are **lazy**: `xs.Where(p)` does no work — it builds a deferred pipeline that runs when enumerated. Unlike elsewhere, a **bound lazy query is affine**: it may be enumerated **at most once** — the first consumption spends its one *enumeration right*, the way a `move` spends ownership. The frame itself is still yours to dispose — `using var` where you want scope disposal, otherwise K3.14's auto-drop, with the must-use warning suppressed wherever the consumption point is statically visible; the right and the disposal obligation are layered, not the same thing (spec §29.11).

```bezel
using var hot := readings.Where(lambda(r) => r.Celsius > 90.0);   // bound lazy query
var n := hot.Count();            // spends its one enumeration right
for var r in hot do              // ERROR: `hot` was already consumed — the
  Plot(r);                       //   diagnostic points at .ToArray()
end;
```

Elsewhere the second loop would silently *re-execute* the pipeline. Bezel refuses, because re-execution interacts with the borrow lifetimes of the query's source and captures. To read results more than once, **materialize**:

```bezel
using var hot := readings.Where(lambda(r) => r.Celsius > 90.0).ToArray();
var n    := hot.Count;                            // a collection property now
var mean := hot.Average(lambda(r) => r.Celsius);  // as many passes as you like
```

`ToArray()` returns an owned `Array<T>` — a collection, enumerable any number of times. Only the bound lazy value is affine. The `owned` is the **container** axis: at class or interface elements the fresh array is a *borrowing* collection whose slots hold borrows of the elements it enumerated (stdlib §5, §4.4; spec §28.11), so materializing answers *re-enumeration* over a live backing and answers nothing about the elements' own lifetimes — that is §6's problem, with §6's remedies. Exactly three uses spend a bound query's enumeration right (spec §29.11):

1. **Enumeration or a terminal operator** — a `for` loop, `Count()`, `First()`, `ToArray()`, ….
2. **Composing a further operator** — the right transfers into the composed query:

   ```bezel
   using var warm  := readings.Where(IsWarm);   // bound
   var names := warm.Select(Label);       // consumes `warm` — the right moves into `names`
   var n     := warm.Count();             // ERROR: `warm` was consumed by the Select
   ```

3. **Passing it as an `IEnumerable<T>` argument** — the right transfers to the callee:

   ```bezel
   method Mean(xs: IEnumerable<double>): double
     return xs.Average();               // one enumeration — all a callee may assume
   end Mean;

   var m := Mean(readings.Select(CelsiusOf)); // unbound temporary: the right passes to the callee,
                                              //   the frame is disposed at statement end
   ```

   Inside the callee a plain `IEnumerable<T>` parameter is statically untracked — the affine tracking follows provenance, not the type, so a violation there is caught by the runtime guard instead: `InvalidOperationException`, the same loud failure as mutation during iteration. Write callees to enumerate a sequence parameter once; a callee that needs two passes materializes.

**Which operators are lazy, and what they retain** (stdlib §5):

- The callback-taking lazy operators — `Where`, `Select`, `SelectMany`, `TakeWhile`, `SkipWhile`, `OrderBy`/`OrderByDescending`/`ThenBy`/`ThenByDescending`, `GroupBy`, `Zip` — **retain their callback in the query value until it is consumed**, and with it every borrow the callback captured.
- The callback-free lazy operators (`Take`, `Skip`, `Reverse`, `Distinct`, `Concat`, `Union`, `Intersect`, `Except`) retain nothing.
- The **eager** operators run during the call and retain nothing: the element accessors (`First`/`Last`/`Single`/`ElementAt`, with the `*OrNull` forms, and the predicate forms where they exist — `ElementAt`/`ElementAtOrNull` carry none), `Count`, `Any`, `All`, `Contains`, the aggregations (`Sum`, `Min`, `Max`, `Average`, `Aggregate`), and the materializers (`ToArray`, `ToDictionary`, `ToSet`).

(On an `Array<T>` receiver, `Reverse()` resolves to the in-place instance method before extensions are consulted — the lazy operator applies only where the receiver has no instance `Reverse`; stdlib §5.)

**Empty sources, while we are near the aggregations** (stdlib §5/§5.1): `Sum` returns the zero of its type and `Aggregate` returns its seed, but `Min`, `Max`, and `Average` throw `InvalidOperationException` — the assert-shaped tier. The branch-shaped tier is `MinOrNull()`/`MaxOrNull()` — no-selector forms only; compose `Select(sel).MinOrNull()` for a keyed one. And `First(pred)` is exactly `Where(pred).First()`, with `FirstOrNull(pred)` as its null-signaled twin.

Retention has one practical consequence: **keep captured sources alive until the consumption point.**

```bezel
using var filter := LoadFilter();                          // owned; the lambda borrows it
var q := readings.Where(lambda(r) => filter.Admits(r));    // q retains the lambda —
using var kept := q.ToArray();                             //   and its borrow of `filter` —
                                                           //   until this consumption
```

The compiler tracks the captures as sources carried by the query value (spec §28.14; stdlib §5), so the failure mode is a compile error, not a dangle. The same tracking draws the return boundary. A method may return a pipeline over its *own inputs* — `static method Evens(xs: Array<int>): IEnumerable<int> => xs.Where(IsEven);` — because each caller re-establishes the borrow against its own argument. A pipeline over a local you created dies with the method and cannot be returned. At value, `string`, and runtime-managed elements, materializing it into an owned `Array<T>` is the fix — the slots are copies or runtime-managed references, and the array carries nothing of the local. At **class or interface** elements it is not: the materialized slots are borrows of elements the local still owns, so the returned array dangles the moment the local is disposed (stdlib §5, v0.265). Return an owning collection you built through §6's owning path, or let the caller own the source.

---

## 5. `GroupBy`: consume the outer sequence once; read each group freely

`GroupBy` is lazy until first enumerated, then buffers fully — no complete group can exist before the source is exhausted. That first enumeration is where it consumes its source (stdlib §5, v0.230). Single-consumption binds to the **outer** `IEnumerable<IGrouping<TKey, T>>` only. Each group is a materialized, **re-enumerable** collection that stays valid after the outer sequence advances past it — the *grouping* survives, though at class or interface elements its buffer holds borrows, so the *elements* survive only as long as their own owner does, and a grouping over a source whose enumerator owns the elements carries dead borrows exactly as a materializer does — and, since v0.268, draws §6's warning at an accessor over the group where that source is same-unit (stdlib §5; §6). Over a collection-backed source the many-aggregations idiom is direct:

```bezel
for var g in requests.GroupBy(lambda(r) => r.Route) do     // outer: consumed by this loop
  Console.WriteLine(
    $"{g.Key}: {g.Count()} calls, slowest {g.Max(lambda(r) => r.Millis)} ms");
end;                                                       // g read twice — defined (v0.230)
```

Groups appear in first-occurrence-of-key order, elements within a group in source order. When a group feeds *many* aggregations, materializing it once — `using var items := g.ToArray();` on the body's first line — remains the advised shape (stdlib §5); it is advice, not a rule.

---

## 6. `First` and friends at class elements: you get a borrow of the element

At value and `string` elements, `First()`, `FirstOrNull()`, `Single(pred)`, `ElementAt(i)`, `Min()`/`Max()` just hand you the value. At **class or interface** elements they hand you a **borrow of the element, sourced from the receiver** — no disposal obligation, usable while the element's backing is live (stdlib §5, scoped at v0.235). Over a collection-backed sequence that backing is the collection:

```bezel
using var fleet := new OwningArray<Connection>();
fleet.Add(Connect("a"));
fleet.Add(Connect("b"));

var idle := fleet.FirstOrNull(lambda(c) => not c.IsBusy());  // borrow of the element
idle?.Ping();                     // fine while fleet lives — fleet is the backing
```

**The trap is a pipeline whose elements the enumerator owns.** Over such a source — an iterator that `new`s its elements, an iterator that binds them to a binding **its own frame owns** (an owned local, a `using var` holding an owned value, or an `owned` parameter of the iterator body) and yields that name, or an operator chain over either — the element lives in the pipeline's *own* enumerator frame, which the terminal operator disposes before returning. The borrow you get back points at a disposed element, and the next use faults deterministically in checked mode (`DisposedAccessException`, spec §28.2). Since v0.243 the compiler **warns at the accessor call** whenever that iterator is compiled in the same unit — since v0.268 for the adopt-then-yield producer as well as the minting one, and through a `ToArray()` or `GroupBy` in the chain (stdlib §5; the predicate is spec §29.11's) — the diagnostic states the fault as certain where every reachable yield of the traced iterator is enumerator-owned and as *may not survive* where the yields are mixed, and it names the remedies below; a cross-module producer escapes the warning and keeps only the checked-mode fault:

```bezel
using var probes := Probes(8);    // Probes is an iterator minting fresh Probe objects
var p := probes.First();          // borrow of the element — compiles, but WARNS
                                  //   (stdlib §5, spec §29.11: same-unit
                                  //    enumerator-owned-yield root)
p.Run();                          // checked mode: DisposedAccessException — the element
                                  //   died with First()'s internal enumerator
```

The `new`-in-the-yield spelling is not the only one. An iterator that binds the element first — the spelling §29.2 calls *warning-free*, because binding it is what silences the **producer's** must-use warning — hands the enumerator's frame the same ownership, and since v0.268 the consumer's accessor warns identically:

```bezel
public static method Load(paths: IEnumerable<string>): IEnumerable<Doc>
  for var p in paths do
    using var d := Parse(p);      // Parse(p: string): owned Doc — frame-owned
    yield return d;               // adopt-then-yield: a borrow of a frame-owned binding
  end;
end Load;

var first := Load(paths).First();  // WARNS (v0.268) — spec §29.11's adopting arm
first.Title;                       // checked mode: DisposedAccessException — §29.7's
                                   //   pass disposed d when First() disposed the enumerator
```

*Warning-free at the producer* and *warned at the consumer* are both true and are about different call sites. What still does **not** warn: yielding a re-borrow of an element the *caller* owns (`for var d in docs do yield return d; end;` — sound), and reaching a frame-owned value through an indexer or a field rather than by name (`yield return seen[i];` — silent by choice, spec §29.11, because a frame-resident *borrowing* collection would be warned on wrongly).

Ownership of the binding is what counts, not its spelling: `using var d := docs[i]; yield return d;` owns nothing — a `using var` initialized from a borrow assumes no obligation (spec §28.10) — so that yield is an ordinary re-borrow and stays silent.

One shape is **over-warned**, and v0.268 says so rather than pretending otherwise (spec §29.11). An adopting yield whose binding the body then transfers out of the frame — `yield return d; sink.Add(move d);` — is dead at an *abandoning* accessor (`First`, `FirstOrNull`, `ElementAt`, which dispose the enumerator at the suspension point) and *live* at an *exhausting* one (`Last`, `Single`, `Min()`, `Max()`, which run the transfer first). The predicate keys on the receiver's provenance, not on which accessor consumes it, so it warns on both and states the fault as certain on both. The remedy is the same one below and worth taking anyway: make the transfer's destination the element's owner, and apply the accessor to *that* collection.

The remedy is the one the stdlib states (§5), and `ToArray()` is **not** it: materializing hands you a fresh `Array<T>` whose slots are *borrows* of the same dead elements (stdlib §5, §4.4) — §4's materialize-first remedy answers re-enumeration, not liveness. Two things work. Use each element inside the loop body that borrows it, before the next advance. Or give the elements an owner: run the producer through the owning path (an `IOwningEnumerable<T>` iterator consumed with §3's owning iteration, where you genuinely own each element) and build them into an owning collection — the plainest form, and then `probes.FirstOrNull(...)` over an `OwningArray<Probe>` is collection-backed and sound. If the producer is a borrowing minting iterator you cannot change, there is no way to keep an element past the next advance; consume it in the body.

Three footnotes close the story. First, the tiers are the usual ones (stdlib §1): `First` throws `InvalidOperationException` on no match — absence is a bug at that call site — while `FirstOrNull` returns `null` for the expected-absence stance (Guide 2 §2). Second, `Aggregate(seed, fold)` at a class-typed accumulator additionally carries `seed` as a source: the result is usable only while receiver *and* seed live (stdlib §5). An empty source returns the seed itself (stdlib §5). Third, these accessors are *compiler-known* at class elements, because no in-language body over arbitrary `IEnumerable<T>` could hand an element borrow out of its loop (§2's escape rule). A first-element helper of your own takes an indexable receiver and returns `this[i]` — the receiver-sourced spelling the rule admits (spec §28.3, v0.235).

---

## 7. Spans and buffers, in brief

For contiguous storage and zero-copy sub-ranges, the stdlib layers three things (stdlib §4.14–§4.16, §3.3, §4.19):

- **`OwningBuffer<T>`** — the storage primitive: a fixed-but-runtime-sized block of slots. Class-typed T: nullable owned slots (`T?`), disposal cascades, `move buf[i]` admitted. Value-typed T: plain value slots, no per-slot disposal. The resizable owning collections are built over it.
- **Spans** — non-allocating **views** into existing storage: `Span<T>`/`ReadOnlySpan<T>` over an `OwningBuffer<T>`, `ArraySpan<T>`/`ReadOnlyArraySpan<T>` over an `Array<T>`, `ReadOnlyStringSpan` over a `string`. Construct with the `AsSpan()`/`AsReadOnlySpan()` family; `Slice` narrows further. Range restriction is the point: a span bounds-checks against *its* length, not the whole source's.
- **`FrozenArray<T>`** — deeply immutable, fixed after construction; T restricted to sendable value types and `string`. The share-forever form: it is sendable, `Shared<FrozenArray<T>>` lets N domains read one buffer with zero bytes copied, and the value-typed `From(move buffer)` freeze is O(1) — it steals the storage (stdlib §4.19; spec §28.13).

```bezel
using var buf := new OwningBuffer<byte>(4096);   // runtime-sized, value slots
var header := buf.AsSpan(0, 64);                 // zero-copy writable view
header.Fill(0);                                  // bulk write through the view

var s := "Content-Type: text/plain";
var kind := s.AsSpan(14);                        // ReadOnlyStringSpan — no allocation
if kind.StartsWith("text/") then Accept(); end;
```

**A span is a borrow of its buffer.** `Span<T>` is a borrow-bearing record (spec §9.7): it cannot outlive the buffer it views, cannot sit in fields or escape into async frames, and cannot be returned over a buffer that dies with the method:

```bezel
method Header(): Span<byte>
  using var buf := new OwningBuffer<byte>(64);
  return buf.AsSpan();           // ERROR: the span borrows buf, disposed at scope end
end Header;
```

The fix is Guide 1 §3's: let the *caller* own the buffer and pass a span down, or return the owned buffer itself. The one exception is deliberate. `ReadOnlyStringSpan` is an ordinary record, not borrow-bearing: its `Source` is a `string`, runtime-managed, and the reference itself keeps the string alive. So string spans may sit in fields and cross `await` freely (stdlib §3.3); `ToString()` materializes a plain `string` when an API wants one.

The payoff of the view types is **reuse without allocation** — one buffer, many operations. The encode idiom (stdlib §3.2/§9.6) is the model:

```bezel
using var scratch := new OwningBuffer<byte>(4096);       // one buffer for the whole loop
for var line in lines do
  var n := Encoding.UTF8.GetBytes(line, scratch.AsSpan());  // zero per-call allocation
  Send(scratch.AsReadOnlySpan(0, n));                       // hand out just the used range
end;
```

Spans iterate — `for var x in span do … end;` works — but deliberately *not* via the enumeration protocol: an enumerator is a heap object that would hold the span's borrow beyond what the tracker can bound, so the compiler emits a direct indexed loop instead (spec §29.2). Consequence: a span is not an `IEnumerable<T>` and cannot be passed as one — pass the span itself (or `IReadOnlySpan<T>`, stdlib §4.16, when abstracting over span sources). The loop variable follows the slot kind, as always: a copy for value T; for class T, a borrow of the slot's value, typed `T?`, since class-typed buffer slots are nullable. And on a writable class-typed span, `move span[i]` takes the slot's element with the same restoration rule as any owning slot — the moved-from state lives in the underlying buffer (spec §28.11, §29.2).

Finally, when data should stop changing and start being shared, **freeze it**:

```bezel
var raw := LoadSamples();                             // owned OwningBuffer<double>
using var input := new Shared<FrozenArray<double>>(
    FrozenArray<double>.From(move raw));              // O(1): the storage is stolen
```

After the freeze no writer can exist — the consume takes the buffer, and the consume-site freeze rejects it while any span over the buffer is still live (spec §28.3; stdlib §4.19). Clone the handle per reader and the last one, wherever it lives, disposes the storage (spec §28.13). Iteration over a `FrozenArray<T>` yields plain copies — there is nothing to own and nothing to invalidate.

---

## 8. Choosing the right tool

| You want to… | Reach for |
|---|---|
| Hold references owned elsewhere | the borrowing family (`Array<T>`, `Dictionary<K, V>`, …) |
| Hold resources this holder disposes | the owning family — `Dispose()` cascades, no destructor |
| Read elements, either family | an `IEnumerable<T>` / `IReadOnlyList<T>` parameter |
| Look at an element | indexer, `Peek`, `GetOrNull` — borrows, no obligation |
| Take one element out | `Pop*` / `Dequeue` / `PopValue` — owned result (`move coll[i]` + restore for replace-in-place) |
| Consume every element | `for var x in coll.Drain() do using var c := move x; … end;` |
| Read query results twice | `.ToArray()` first — a bound query is single-consumption |
| Aggregate one group many ways | `GroupBy` — outer sequence once, each group re-enumerable |
| Keep an element past the loop | a concrete collection-backed source (materializing gives one only where the elements' own owner outlives the use); the owning path when elements are minted |
| Tally occurrences | `Counter<K>` (stdlib §4.20) — not a dictionary dance |
| Zero-copy sub-range | spans via `AsSpan()` — bounded by the source's life (string spans exempt) |
| Share bulk data across domains, forever | `FrozenArray<T>`, under `Shared<T>` |

And the five traps, each a section above: **enumerating a bound query twice** (§4 — materialize); **escaping an iteration variable** from an interface-typed or minting source (§2 — concrete backing, or the owning path); **disposing a borrowed element** (§2 — only owners dispose; go through the owning collection or take the element out); **`First()` over a pipeline whose enumerator owns the elements** (§6 — warned since v0.243 where the producer is same-unit, widened at v0.268 to adopt-then-yield producers and to chains through `ToArray()`/`GroupBy`, spec §29.11; materializing does not help — give the elements an *owner*, or use each one in the loop body that borrows it); **holding a span past its buffer** (§7 — the caller owns the storage).

Three habits carry most of it. Pick the family that states who disposes, and let `Dispose()` cascades and `Drain()` do the element work — hand-written disposal loops over collections are almost always the wrong shape. Treat everything a loop or accessor hands you at class elements as a borrow unless the name says transfer (`Pop`/`Drain`). When you need it longer, take ownership explicitly rather than fighting the borrow checker. And treat a lazy query as a value you spend: bind it, consume it once, and reach for `ToArray()` the moment you need the results twice. The compiler enforces all three; the errors are it finding a leak, a dangle, or a silent re-execution before they happen.
