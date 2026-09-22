# Proposal 0130 — `OwningSlotMap<T>`: an owning collection keyed by generational handles

| | |
|---|---|
| **Status** | under-review — owner-directed 2026-09-22: drafted for graduation in the v0.307 train; adversarial critique pending |
| **Tracking issue** | to be filed at landing |
| **Target module(s)** | **stdlib** — new §4.21 `OwningSlotMap<T>` and `SlotHandle` (after §4.20 `Counter<K>`); §4.3's `Clear()`-order paragraph (one family added). **spec** — §2.3's `System.Collections.Generic` auto-import row (the bare-name list and its count); §28.11's owning-family list, its disposal-order paragraph, and its no-indexer paragraph; §28.9's non-trivial-dispose list. **rationale** — §5.4 (one dated sentence: the handle-and-table pattern it names is now a library type). **code-generation reference** — §4.5's family table, the §5.4 index (one new block), §5.11's collection absences, and the pin. **Guide 05** — §8's table (one row). No grammar change. |
| **Written against** | spec **v0.306** (`cafdfc5`); stdlib companion v0.306 |
| **Origin** | The 2026-09-22 "path of least resistance" investigation: make the good memory-management pattern the shortest spelling; make the compiler say what it already knows. This is the first half — a stored *key* instead of a stored *borrow* becomes the shortest spelling for graph-, cache- and registry-shaped code. |

*Reference convention: unprefixed §-references are language-spec references; stdlib, rationale and guide references are marked; references to this proposal's own sections are written "this proposal's §N". Quotations are drawn at v0.306 and are verbatim; bold inside a quotation is the source's own. ▲ marks proposed surface, landed nowhere.*

---

## 0. Thesis

Bezel's answer to "many places need to refer to one object" is, in the rationale's own words, *"handle-and-table indirection"* (rationale §5.4) — and the language ships no table for it. Today the pattern is hand-rolled from `OwningDictionary<int, T>` plus a counter, or from an `OwningBuffer<T>` plus an index, and the second form has the defect the type below exists to retire: a stale index after a removal silently reads whatever now occupies the slot. The alternative the documents teach for the removal case — `Shared<T>`/`Weak<T>` — is reference counting, which rationale §5.21 records was added *because* "the workarounds (handle tables) push the same reference counting into user code where the compiler cannot see it". This proposal takes the other branch of that sentence: a table the compiler *can* see, whose keys are values (no borrow, no count), and whose staleness is detected rather than silently reused.

`OwningSlotMap<T>` ▲ is an owning collection (spec §28.11's owning family, one more named type under axiom A9/A14) whose ingress mints a **`SlotHandle`** ▲ — a small sendable value carrying a slot index and a generation — and whose every read is keyed by that handle. A removed slot's generation advances, so a handle minted before the removal reads **absent** for the rest of the map's life; a handle from another map is a **fault**. Storing a `SlotHandle` in a field, a record, or a collection carries no borrow (it is a value of primitive components), no K4.3 trust clause, and no domain clause — which is what makes it the shortest correct spelling.

## 1. The current state, precisely

**What the documents say the pattern is.** Rationale §5.4: *"The cases where shared ownership is genuinely needed are rare in Bezel's target use cases and are served by other patterns: handle-and-table indirection, channels, or the runtime-managed region for shared immutable data."* Rationale §5.21, on why `Shared<T>` was added: *"caches handing out entries, registries whose entries outlive any single owner, and the multi-consumer channel question (§12.6) all reduce to 'N holders, last one cleans up,' and the workarounds (handle tables) push the same reference counting into user code where the compiler cannot see it."*

**What a stored borrow costs.** §28.3's borrowed-field store rule, clause 3: *"**The residual.** The residual is a [K4.3]-recorded trust clause: whether the caller-side source actually outlives the object is the programmer's obligation, not a compile-time guarantee, with [K4.1]'s backstop as the named safety net."* Clause 5 extends the same residual — and its domain clause — to *"collection-held borrows ([§28.11]'s store rule)"*. Every graph edge, cache back-pointer and registry entry spelled as a borrow is one of these clauses.

**What exists today, and the hand-rolled forms.** Stdlib §4.9 declares `OwningDictionary<K, V>` (*"Owning hash-based map. Owns its values; keys are value-like"*) with `Add(key: K, owned value: V)`, `TryAdd`, `Remove(key: K): bool` (*"disposes the removed value"*), `TryGetValue(key: K, out value: V?): bool` (*"out is borrow"*), `GetOrNull(key: K): V?` (*"a look, not a take"*), `PopValue`/`TryPopValue`, `Drain(): IOwningEnumerable<DrainedEntry<K, V>>`, `Clear()`, and *no* default entry iteration. Stdlib §4.14 declares `OwningBuffer<T>`, which at class-typed `T` stores `T?` slots and admits `move buf[i]`. The two hand-rolled forms:

```bezel
// (a) the counter form — correct, never reuses a key, pays a hash per access,
//     and cannot back a dense side table
public class NodeTable
  private owned _nodes: OwningDictionary<int, Node>;
  private _next: int;

  public constructor()
    _nodes := new OwningDictionary<int, Node>();
    _next := 0;
  end constructor;

  public method Add(owned node: Node): int
    var id := _next;
    _next := _next + 1;
    _nodes.Add(id, move node);
    return id;
  end Add;

  public method Get(id: int): Node? => _nodes.GetOrNull(id);
end NodeTable;

// (b) the index form — dense and cheap, and a stale index after Remove
//     silently reads the slot's NEW occupant
public class NodeSlots
  private owned _slots: OwningBuffer<Node>;
  private owned _free: Array<int>;
  // Remove(i): `_slots[i] := null;`, `_free.Add(i);`
  // Add(node): pops a free index and stores; an old `i` handed out before
  //   the Remove now reads the new node — no fault, no null, no diagnostic
end NodeSlots;
```

Form (a) is what the stdlib's own §2.16 example gestures at (`_index: OwningDictionary<string, Shared<ParsedConfig>>`); form (b) is the one whose defect is silent. Neither is *wrong* under the spec; both are ceremony for a pattern the rationale names as the sanctioned one.

**Absence, checked (process §5 item 3b).** `grep -rn -i 'SlotMap\|OwningRegistry\|OwningTable\|generational' src/ docs/ tests/` → no hit outside this proposal (the only `slotMap` in `src/` is a codegen-local variable name in `LlvmBackend_Assign.cpp:6316`, unrelated). No compiler support for a handle-keyed owning collection exists to be recorded as impl-ahead-of-spec.

## 2. The design

### 2.1 The names

Two candidates in the `Owning…` family (stdlib §4's convention: the prefix names the **element** axis, rationale §3.9):

- **`OwningRegistry<T>` / `RegistryHandle`** — names the *intent* (a place things are registered and later found), the way `Counter<K>` names a tally rather than a map. Declined: "registry" is already this project's word for a *different* shape — §22.12 rule 4(a)'s *"`static owned readonly` being the canonical process-global registry idiom"* over `Locked<T>`, the spec's own `Registry<int>.Register()` example in §22.12, and rationale §16.7's per-invocation "registry" for `ForeignHandle<T>`. A stdlib `OwningRegistry` would collide with all three in a reader's head, and it says nothing about *how* the type finds things.
- **`OwningSlotMap<T>` / `SlotHandle`** — names the *mechanism*: a map from slots to values whose keys the map itself mints, the generational-index structure a reader will search for under exactly this name. "Map" places it beside `Dictionary` (the keys are minted rather than chosen — that is the one difference, and the handle's name carries it); "Slot" is §28.11's own word for an element location. **Chosen.**

The handle is `SlotHandle`, not `SlotKey`: a key is something the caller chooses; a handle is something the collection hands out.

### 2.2 `SlotHandle` ▲ — the key type

A compiler-known **value record**, on the `CancellationToken`/`Guid` pattern (stdlib §1's compiler-intrinsic convention): no public constructor, two public read-only components, equality and hashing compiler-provided.

```bezel
/// Compiler-intrinsic value record. Sendable (§30.14); hashable (§3.6) — usable as a
/// Dictionary<K, V> key or Set<T> element; not serialization-eligible (§9.8).
public record SlotHandle
  public readonly property Index: int;         // the slot's position in its map; stable for the handle's life
  public readonly property Generation: int;    // the slot's generation when the handle was minted
  // =, <>, Equals, GetHashCode and ToString are compiler-provided. Two handles are equal
  // iff they were minted by the same map for the same slot at the same generation.
  // ToString renders `SlotHandle(Index: i, Generation: g)` (the §9.1 positional shape).
end SlotHandle;
```

Design points, each decided:

- **A record, not a compiler-known opaque scalar.** A record is what §9 already gives the language for "a small value with components and value equality"; §30.14's value-type rule makes it sendable for free (all components primitive); §3.6's hashable property makes it a `Dictionary`/`Set` key for free. An opaque scalar would need every one of those stated by hand.
- **Why `Index` and `Generation` are public.** `Index` is the *dense side-table* affordance — the reason slot maps exist in game engines and compilers: `Array<Meta>` indexed by `h.Index`, kept beside the map. `Generation` is the debugging affordance (a stale handle is visibly stale). Neither confers authority: a handle is a key, not a capability, so exposing its components costs no soundness (this proposal's §2.4).
- **Why no public constructor.** A handle denotes a slot of one map; a user-constructed one would denote nothing. The compiler-known record has no constructor spelling, exactly as `CancellationToken` has none beyond `None`.
- **The hidden owner tag.** The record carries a third, non-public component: the identity of the map that minted it — a process-unique `long` the runtime assigns to each `OwningSlotMap<T>` at construction from a monotonic counter (the same class of runtime bookkeeping as §8.3's identity hashes, rationale §12.11; not an intercept point, stdlib §1). It participates in equality and hashing and is never observable except through them and through this proposal's §2.4 fault. It is what lets a handle from another map be a *fault* rather than a *miss*.
- **Not serialization-eligible.** A handle is meaningful only against the live map that minted it in the process that minted it; a serialized handle would be a forged one on arrival. `Index`/`Generation` may be read out and stored by a program that knows what it is doing; the record itself declares no `ISerializable` conformance and §9.8 synthesizes none for a compiler-known type that opts out.

### 2.3 `OwningSlotMap<T>` ▲ — the collection

```bezel
public class OwningSlotMap<T>: IEnumerable<T>, IOwningEnumerable<T> where T: class
  public constructor();
  public constructor(capacity: int);                      // ArgumentOutOfRangeException if negative (§1)

  public pure readonly property Count: int;               // live slots

  public method Insert(owned value: T): SlotHandle;       // takes ownership; mints the handle
  public method Contains(handle: SlotHandle): bool;       // live under this handle; enables `h in map` (spec §17.5)
  public method Get(handle: SlotHandle): T;               // borrow of the stored value; KeyNotFoundException if absent
  public method GetOrNull(handle: SlotHandle): T?;        // §1 *OrNull tier: absent -> null; present -> a borrow
  public method TryGetValue(handle: SlotHandle, out value: T?): bool;   // §1 shape (a); the out is a borrow
  public method Remove(handle: SlotHandle): bool;         // disposes the value; false if absent
  public method PopValue(handle: SlotHandle): owned T;    // vacates the slot; KeyNotFoundException if absent
  public method TryPopValue(handle: SlotHandle, out owned value: T?): bool;   // §1 shape (a)
  public method Handles(): owned Array<SlotHandle>;       // a snapshot of every live handle, ascending Index
  public method Drain(): IOwningEnumerable<T>;            // iteration drains the map, ascending Index
  public method Clear();                                  // disposes every value, descending Index
end OwningSlotMap;
```

The normative usage notes (the stdlib §4.21 text in `scratch/train-B/edits.md` carries these verbatim):

1. **Slots and generations.** The map is a table of slots addressed by index. Every slot carries a **generation**, `0` when the slot is first used and **advanced by one each time the slot is vacated** — by `Remove`, `PopValue`, `TryPopValue`, a `Drain()` yield, or `Clear()`. `Insert` places the value in a vacant slot and mints a handle carrying that slot's index and *current* generation. A handle is **live** iff its slot holds a value and the slot's generation equals the handle's `Generation`; otherwise the handle reads as **absent** — `Contains` is `false`, `GetOrNull` is `null`, `TryGetValue`/`TryPopValue` return `false`, `Remove` returns `false`, and `Get`/`PopValue` raise `KeyNotFoundException`. A handle therefore never reads a later occupant of its slot.
2. **Reuse and retirement.** `Insert` reuses the **most recently vacated** slot that is not retired, and otherwise takes the next never-used index; `Index` values are consequently dense and small, which is what makes them usable as side-table indices. A slot whose generation would exceed `int.MaxValue` is **retired** — never reused — so generations never wrap and a stale handle can never become live again.
3. **A handle from another map is a fault.** Every handle-taking member raises `ArgumentException` when the handle was minted by a different `OwningSlotMap<T>` instance — stdlib §1's band 1: a caller bug, not an expected absence.
4. **Ownership faces** (spec §28.11's owned column, verbatim for this type): `Insert` takes `owned` — a fresh producer or a `move`d binding, never an unmarked read of an owned binding (K3.3); `Get`, `GetOrNull`, `TryGetValue`'s out, and default iteration produce **borrows sourced from the map**, valid until the slot is mutated (the producers-table indexer row's owning-family qualification; a `Remove`/`PopValue`/`Clear` on the slot is a mid-scope kill under the checked-mode backstop); `Remove` and `Clear` **dispose**; `PopValue`, `TryPopValue` and `Drain()` **transfer**. Disposing through a borrow obtained from `Get`/`GetOrNull` is §28.6's compile error.
5. **Order.** Default iteration (`IEnumerable<T>`), `Handles()` and `Drain()` visit live slots in **ascending `Index`**. `Clear()` and the map's own `Dispose()` dispose the values in **descending `Index`** — the ordered families' reverse-storage-order rule of §28.11, storage order here being slot order. `Clear()` keeps the emptied map; `Dispose()` then releases the storage.
6. **`Handles()` is a snapshot, not a view.** It returns a fresh `owned Array<SlotHandle>` — trivial-dispose at this element type, so a `for var h in map.Handles() do … end;` loop carries no ceremony — because the dominant loop over a registry *removes as it goes*, and §29.12 makes mutation through a live view raise `InvalidOperationException`. (§4.9's `Keys` is a live view because its example collects keys into an array first; this type provides the array directly.)
7. **Mutation during iteration** is §29.12's rule: `Insert`, `Remove`, `PopValue`, `Drain` and `Clear` are structural and invalidate an active enumerator.
8. **Interface conformance.** `IEnumerable<T>` (borrowing default iteration) and `IOwningEnumerable<T>` (the `Drain()` path), the both-implemented shape of `OwningArray<T>` (§4.13). **Not** `IOwningCollection<T>`: that interface's `Add(owned item: T)` returns `()` where this type's ingress must return the handle, and its `Contains(item: T)`/`Remove(item: T)` are keyed by element identity where this type's are keyed by handle; a conformance would declare a second, weaker access face beside the one the type exists to provide.
9. **No collection literal.** The type declares neither `ICollectionLiteralTarget` nor `IOwningCollectionLiteralTarget` (§15.4.1, stdlib §2.19): a literal cannot hand back the handles it would mint, and an insertion that discards its handle is the one thing the type is for not doing. `new OwningSlotMap<T>()` followed by `Insert` is the construction form.
10. **No borrowing twin.** There is no `SlotMap<T>` storing borrows: handles exist to *replace* stored borrows, and a borrowing form would reintroduce at every slot exactly the §28.11 store residual the handle removes. The map owns; a program that wants to refer to an object owned elsewhere stores that object's handle, not a borrow of it.
11. **Derived properties.** Not trivial-dispose at any instantiation (§28.9: an owning collection cascades). Sendable iff `T` is a sendable class type (§30.14's owning-collection rule: the state — values, slot table, free list, owner tag — is wholly owned; handles left behind in the origin domain denote a map that domain can no longer reach, which is sound by construction). Declares no statics-and-sharing facts (stdlib §1): a `static owned readonly` map draws §22.12 rule 4's warning like every other owning collection, and the `Locked<T>` registry idiom is the process-global spelling.
12. **Element restriction.** `where T: class`, enforced at the instantiation site exactly as for every owning collection (§14.4's class-only restriction; interface and nullable-underlying types admitted).

### 2.4 Why a handle carries no obligation and no trust

A `SlotHandle` is a value of primitive components. Stored in a field it is a copy slot (§28.11's value column) — not a borrow, so §28.3's borrowed-field store rule, its K4.3 residual and its domain clause do not attach. Stored in a borrowing collection it is a copy (§28.11 copy slots). Captured by a closure, sent across a domain, returned from a static method: a value everywhere. Nothing about the *handle* ever needs the checked-disposal backstop; what the backstop still covers is the *borrow a read produces* (`Get`/`GetOrNull`), whose life is the same as any owning-collection element borrow's — bounded by the map and by the next mutation of the slot.

Forging (`Index`/`Generation` read out and reassembled) is impossible at the surface — there is no constructor — and would be harmless if it were: a handle confers no ownership and no access the map does not already check. The one thing a handle *does* carry is the owner tag, and that is checked, not trusted (this proposal's §2.3 item 3).

### 2.5 The idiom, in full

```bezel
public class Graph
  private owned _nodes: OwningSlotMap<Node>;
  private owned _edges: Array<Edge>;             // record Edge(From: SlotHandle, To: SlotHandle)

  public constructor()
    _nodes := new OwningSlotMap<Node>();
    _edges := new Array<Edge>();
  end constructor;

  public method AddNode(owned n: Node): SlotHandle => _nodes.Insert(move n);

  public method Link(a: SlotHandle, b: SlotHandle)
    _edges.Add(new Edge(a, b));                  // two copies; no borrow, no trust clause
  end Link;

  public method RemoveNode(h: SlotHandle)
    _nodes.Remove(h);                            // disposes the node; every Edge naming h now reads absent
  end RemoveNode;

  public method Neighbours(h: SlotHandle): owned Array<SlotHandle>
    using var found := new Array<SlotHandle>();
    for var e in _edges do
      if e.From = h and _nodes.Contains(e.To) then found.Add(e.To); end if;
    end for;
    return move found;
  end Neighbours;
end Graph;
```

Every reference in `Graph` is either an `owned` field or a value. There is no destructor, no back-reference, no `Shared<Node>`, and no K4.3 clause anywhere in the class.

## 3. Traced examples, and the shapes that must NOT change

Traced against §28.3/§28.11 at v0.306 and this proposal's §2.3:

| Shape | Verdict | Why |
|---|---|---|
| `var h := map.Insert(new Widget("a"));` | accepted, `h` a value local | fresh producer at an `owned` position, marker-free (K3.13) |
| `var w := new Widget("a"); map.Insert(w);` | **rejected** | unmarked read of an owned binding at an `owned` argument — K3.3 requires `move w` |
| `map.Insert(move w);` | accepted; `w` moved-from | K3.3 |
| `using var w := new Widget("a"); map.Insert(move w);` | **rejected** | A5: `using var` is never movable out — unchanged |
| `var v := map.GetOrNull(h); v!.Dispose();` | **rejected** | §28.6: `Dispose()` through a borrow (a call result) — unchanged |
| `map.Remove(h);` | accepted; disposes | the owned column's `Remove` |
| `using var taken := map.PopValue(h);` | accepted; `taken` owned | transfer out |
| `for var v in map do v.Ping(); end for;` | accepted; `v` a borrow | `IEnumerable<T>` default path, as `OwningArray<T>` |
| `for var v in map.Drain() do using var x := move v; … end for;` | accepted; `v` owned per iteration | `IOwningEnumerable<T>` path, §4.13 |
| `for var h in map.Handles() do map.Remove(h); end for;` | accepted | the snapshot is a fresh trivial-dispose array, not a view — no §29.12 fault |
| `for var v in map do map.Remove(someHandle); end for;` | compiles; **`InvalidOperationException`** at the next advance | §29.12 |
| `record Edge(From: SlotHandle, To: SlotHandle);` stored in an `Array<Edge>` field | accepted, no warning | value components; not borrow-bearing (§9.7) |
| `Domain.Spawn(W, move map)` at sendable `T` | accepted | §30.14 owning-collection rule |
| `map2.Contains(h1)` for `h1` from `map1` | compiles; **`ArgumentException`** | this proposal's §2.3 item 3 |
| `[ … ]` with target `OwningSlotMap<T>` | **rejected** | not a literal target (§15.4.1) |

**What must not change:** nothing in the existing owning family moves. `OwningDictionary<K, V>` keeps its user-chosen keys, its `DrainedEntry` drain and its no-default-iteration ruling; `OwningArray<T>` keeps positional indexing and `move arr[i]`; `Shared<T>`/`Weak<T>` keep their §28.13 semantics — `Weak<T>.TryUpgrade` remains the answer when the *holder* must keep the object alive, where a handle is the answer when the *map* keeps it alive and the holder only refers. §28.11's `move coll[i]` list is unchanged: the new type has no indexer and therefore no slot-move form and no restoration rule (its take forms vacate the slot in one step, like `Pop*`).

## 4. Alternatives considered and declined

`DECLINED.md`'s index was checked first. No row records a decline of a handle-keyed owning collection. Three rows are adjacent and untouched: *Allocators and arena allocation, user-facing* (`deferred`, rationale §18) — a mechanism for where allocations *come from*, which this type is not (values are ordinary `new` productions the map takes ownership of; no allocator is supplied, no unit free of foreign allocations exists — the name "arena" is avoided precisely so the two are not confused); *`ForeignHandle<T>`* (`deferred`) — an object↔`IntPtr` bridge for the FFI, unrelated; *Move witness with fix-up (a runtime registry keyed on a value's storage location)* (`declined-at-proposal`) — this type keys on nothing's address (a handle is an index, and the map's storage may move under `Insert` growth with no observable effect).

1. **`OwningDictionary<int, T>` plus a counter is enough (status quo).** Declined: it is the form (a) of this proposal's §1 — correct, but a hash per access, no reuse, no dense index for side tables, and five lines of ceremony per registry that every graph, scene, session table and cache pays again. The rationale names the pattern as the sanctioned one and the library should spell it.
2. **A generational `Weak<T>`-style handle over `Shared<T>`.** Declined: it is reference counting (rationale §5.21's accepted hazard: cycles leak, the compiler does not detect them), and it makes the *holder* of the handle a participant in the object's lifetime. A slot map keeps one owner (the map) and keeps handles inert.
3. **A borrowing `SlotMap<T>` beside the owning one** (the family's usual pair). Declined with reasons in this proposal's §2.3 item 10: a borrow-storing slot map would carry §28.11's store residual per slot, which is the thing the handle exists to remove; the missing twin is not an omission but the point.
4. **Foreign handles read as a miss, not a fault.** Declined: a miss would reproduce, across two maps, exactly the silent-reuse defect the type retires within one; the check is an integer compare against a tag the map already needs for nothing else, and stdlib §1 classifies misuse of another table's key as band 1.
5. **A `[h]` indexer with `get`/`set(owned)`, and therefore `move map[h]`.** Declined for v1: the indexer would put the type on §28.11's `move coll[i]` list and bring the per-slot restoration rule with it, keyed on a *value-typed* index expression the tracker does not classify today (it tracks constant and local-read indices; a handle read is a local read of a record — decidable, but new). The take forms `PopValue`/`TryPopValue` vacate in one step and cover the replace-in-place shape as `PopValue` then `Insert` (a *new* handle — replace-in-place under the *same* handle is recorded as the one operation v1 omits; a `Replace(handle, owned value): bool` member is the natural addition if evidence arrives).
6. **Handles as `int` (the bare index).** Declined: an `int` handle is form (b) of this proposal's §1 — the silent-reuse defect — and it would be freely forgeable and freely confusable with every other `int`.
7. **Insertion-ordered disposal.** Declined on the same ground `DECLINED.md` records for the ordered owning collections (*Per-element arrival tracking in the ordered owning collections*, v0.302): the type stores values, not arrival order; slot order is observable, computable and cheap, and reuse makes insertion order unrecoverable from the map.

## 5. Kernel and body-locality classification

**Kernel.** A **derivation**, no axiom extension: the type's element semantics are §28.11's owned column verbatim (K1 per slot; K3.1 at `Insert`; the transfer rows for `PopValue`/`Drain`); its reads are receiver-sourced borrows (K1.5, the producers table's owning-family indexer qualification); its handles are value-typed and carry no classification (K1.6). The family it joins is axiom A9's owning family and A14's named-types rule; no new axiom is minted and no K4.3 clause is added — the type *removes* instances of the existing ones. Not a §28/§30.5 rule change, so no kernel-register row is owed; the classification is stated because the type is the first stdlib collection whose *purpose* is stated in kernel terms.

**Body locality (proposal 0105 §1).** **Local.** No new compile-time rule; the instantiation-site checks are §14.4's and §3.6 rule 2's existing per-instantiation evaluations over declared structure.

## 6. Migration and compatibility

- **Newly defined:** `OwningSlotMap<T>` and `SlotHandle` (stdlib §4.21), their two bare names on §2.3's auto-import row (twenty-nine → thirty-one), the type's row in §28.11's family list and disposal-order clause.
- **Newly rejected:** nothing that compiles today. The two new bare names shadow-or-ambiguate exactly as §2.3's row states for every name on it: a user module exporting `SlotHandle` or `OwningSlotMap` makes the bare name ambiguous in a file importing both. `grep -rn 'SlotHandle\|OwningSlotMap' tests/ docs/` → no user declaration of either name in the tree.
- **Newly warned:** nothing.
- **Doc-only:** rationale §5.4's dated sentence; the code-generation reference's index block and §5.11 line; Guide 05's table row.

## 7. Implementation sketch

By analogy to `OwningDictionary<K, V>` and `OwningArray<T>`, which the compiler implements as compiler-known heads with a C runtime backing:

- **Runtime** (`src/runtime/bezel_runtime_collections.cpp`, `src/runtime/bezel_runtime.h`): a `bz_slotmap_t` — a `void*` value array, an `int32` generation array, an `int32` free-list stack, a `int64` owner tag from a process-wide atomic counter, `count`, `mod_count` (§29.12; the existing `bz_dict_t` at `bezel_runtime_collections.cpp:455-470` carries the same `mod_count`/`disposed` fields), and the checked-mode `disposed` flag. Entry points mirror the dictionary's disposing family: `bz_slotmap_insert`, `bz_slotmap_get` (index+generation+tag check), `bz_slotmap_remove_disposing(dispose_val)` (the `bz_dict_remove_str_disposing` shape at `bezel_runtime.h:287-291`), `bz_slotmap_pop`, `bz_slotmap_clear_disposing` (descending index), `bz_slotmap_free_disposing`, `bz_slotmap_handles` (ascending snapshot into a fresh `bz_array_t`), the drain enumerator (the §4.9 `Drain()` precedent at `bezel_runtime_collections.cpp:1352`).
- **Builtin registry** (`src/common/BuiltinRegistry.cpp`): `OwningSlotMap` joins `kOwningCollectionHeads` (`:30`), `kOwningTypeHeads` (`:43`), `kHeapPointerReferenceBuiltins` (`:246`), the disposable heads (`:1066`), the constructor-parameter table (`:696`, `capacity`), and the module table (`:1274`, `System.Collections.Generic`); `SlotHandle` joins the compiler-known value records beside `CancellationToken`.
- **Sema** (`src/sema/ExprTypecheck_Calls.cpp`): the member tables keyed by collection head — the `OwningDictionary` blocks at `:5854-5882` (owning ingress), `:7880-7961` (`GetOrNull`/`Drain` result classification: *"the result is a BORROW of the stored value"*), `:12685-13021` (keyed take forms `PopValue`/`TryPopValue`), `:13112-13148` (`Add(owned)` argument classification, fix #4324), `:20037-20122` (the per-family member list) — each gains an `OwningSlotMap` arm with the §2.3 signatures; `SlotHandle` gets a member table (`Index`, `Generation`) and the compiler-provided equality path the other intrinsic records use.
- **Ownership** (`src/ownership/Ownership_Checks.cpp`): nothing new — `Insert` is an owned-argument position (K3.3, the existing owned-ingress classification the `OwningDictionary` arm at `:13138` already selects), reads are receiver-sourced borrows, and a `SlotHandle` local is value-typed and unclassified. `checkCollectionBorrowStoreEscape` (`:8131`) excludes owning heads by name (`:8155-8159`) and will exclude this one.
- **Codegen** (`src/codegen/LlvmBackend_ElementPolicy.cpp`, `LlvmBackend_Disposal.cpp`): the owning-slot dispose cascade keyed on the head name, in descending index order; the `for` desugaring over `IEnumerable<T>`/`IOwningEnumerable<T>` as for `OwningArray<T>`.
- **Sendability** (`src/sema/Sendability.cpp`): the owning-collection rule by head name.
- **Tests:** spec-cited behavior tests on the `runAot` harness for each `run` row of this proposal's §8, plus the two `reject` rows as lit/FileCheck diagnostics.

Size: **M** (a new runtime container plus the per-family sema arms; no new checker rule).

## 8. Corpus accounting

New cases under `tests/conformance/rules/cases/`, `doc: stdlib`, `section: 4.21`, each quoting the §4.21 sentence it pins:

| Case | Expectation | Pins |
|---|---|---|
| `l4_21_0001` | `run`, `output:` | `Insert` mints a handle; `GetOrNull` reads the value as a borrow; the map's disposal cascades (`a` / `disposed a`) |
| `l4_21_0002` | `run`, `output:` | `Remove` disposes and returns `true`; the handle then reads absent; `Count` is `0` |
| `l4_21_0003` | `run`, `output:` | a stale handle after slot reuse reads absent: `Contains(h1)` is `False`, `Contains(h2)` is `True`, `h1 <> h2`, and the slot was reused (`h1.Index = h2.Index`) |
| `l4_21_0004` | `run`, `output:` | `Clear()` disposes in descending `Index` and keeps the map usable (`disposed c` / `disposed b` / `disposed a` / `cleared 0` / `disposed d`) |
| `l4_21_0005` | `run`, `output:` | `Dispose()` disposes in descending `Index` (`disposed c` / `disposed b` / `disposed a` / `after`) |
| `l4_21_0006` | `run`, `output:` | `Drain()` yields owned values in ascending `Index` and vacates as it goes |
| `l4_21_0007` | `run`, `output:` | a handle from another map raises `ArgumentException` |
| `l4_21_0008` | `reject`, `diagnostic: move` | an unmarked read of an owned binding at `Insert`'s `owned` position is rejected (K3.3) |
| `l4_21_0009` | `quiet` | the `move`d twin of 0008 compiles warning-free |
| `l4_21_0010` | `reject`, `diagnostic: borrow` | `Dispose()` through the borrow `GetOrNull` returns is §28.6's compile error; its accepting twin is 0002's `Remove` |

`names.py` will report `OwningSlotMap`/`SlotHandle` as unknown until the stdlib text lands — expected for a case written against a proposed section; the coordinator lands cases and text in one commit.

## 9. Open questions

- **Q1 — `TryGetValue` beside `GetOrNull`.** Kept for §4.9 regularity; at `where T: class` the `T?` never collapses, so the Try form distinguishes nothing `GetOrNull` cannot. Is regularity worth the member, or does the no-dual rule (stdlib §1) want it dropped? Recommended: keep — every keyed map in §4 declares both.
- **Q2 — LIFO reuse as a normative order.** Pinning most-recently-vacated-first makes `Index` values predictable (case 0003 observes it). The alternative is "implementation-defined among vacant slots", which keeps the same guarantees and frees the runtime. Recommended: pin — determinism (§30.13) and the side-table use both want it.
- **Q3 — the owner tag's cost.** One `long` per handle (three words) and one atomic increment per map construction. If the critic finds it disproportionate, the fallback is a two-word handle and foreign use as an unspecified miss, recorded as such — the design is otherwise unchanged.
- **Q4 — `Replace(handle, owned value)`.** Omitted from v1 (this proposal's §4 item 5); the first user who writes `PopValue` then `Insert` and has to re-key every side table will ask for it.
- **Q5 — Guide 05's pin.** The table row is a guide edit riding a normative train; the coordinator decides whether the pin moves to v0.307 with a re-trace or the row lands under the v0.291 pin as a guide-only pass.
