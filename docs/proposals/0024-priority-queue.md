# Proposal 0024 — `PriorityQueue<TElement, TPriority>` (borrowing form)

| | |
|---|---|
| **Status** | under-review — draft |
| **Tracking issue** | [#1451](https://github.com/s8lap/bezel-lang/issues/1451) |
| **Target module(s)** | `System.Collections.Generic` (stdlib §4) — new §4.19 |
| **Written against** | spec **v0.177**; stdlib companion v0.165 |
| **Origin** | Gap review [#1403](https://github.com/s8lap/bezel-lang/issues/1403) item #11 (P0), wave D3 of the [stdlib gap DESIGN plan](stdlib-gap-design-plan-2026-07.md) |
| **Scope** | Borrowing form only. The owning form (`OwningPriorityQueue<TElement, TPriority>` where `TElement: class`) is explicitly deferred to the #1334 two-axis review, which governs the owning-collection family shape. |
| **Currency note** | Refreshed 2026-08-11 to v0.246: `auto-hashable` vocabulary (v0.242; formerly *auto-equality-bearing*). Re-verified at v0.306 (2026-09-17 sweep): no tier-1 respells — both stdlib §4.17 quotations ("there is no separate comparer parameter…", "iteration visits entries in ascending key order") still match the landed text, and `Array<T>`'s `constructor()` / `constructor(capacity: int)` pair is a recorded keep under v0.290's overload-pair rule, so D1/D5 and the §4.19 constructor pair stand as written. ⚠ **One tier-2 flag** recorded in `proposals-currency-2026-09-17.md` §3: v0.282's comparer-injection reconciliation (rationale §17.14) re-grounds the single-order rule as *identity must not fork*, which is the premise D1's decline rests on. Written-against pin unchanged. |

---

## 1. The problem

There is no min-heap in the stdlib today. Common tasks that require one — Dijkstra/A\* pathfinding, task scheduling by deadline, topological priority processing, bandwidth scheduling — must implement a heap from scratch or use a sorted structure with higher asymptotic cost. `PriorityQueue<TElement, TPriority>` fills this gap: a min-heap providing O(log n) enqueue and dequeue with O(1) peek.

The borrowing restriction (elements stored by reference, caller retains ownership) means this proposal is unblocked today. The owning form rides #1334.

---

## 2. Design decisions

### D1 — Priority ordering: `IComparable<TPriority>`, no separate comparer

**Decision.** `TPriority` is constrained `where TPriority: IComparable<TPriority>`. No separate comparer parameter or comparer constructor overload is provided in v1.

*Basis.* `SortedDictionary<K, V>` (§4.17) establishes the single-order rule for comparison-ordered collections in Bezel: `where K: IComparable<K>`, no comparer parameter. The ordering lives on the type; a type that needs a different order wraps its key/priority in a record with the desired `CompareTo`. The same rule applies here — a comparer parameter would be the declined alternative (see §D1 in §4.17's text: *"there is no separate comparer parameter; a type that needs a different order wraps its key"*). Recording the decline here: a `Comparer` constructor overload adds a second ordering contract to manage and lets two queues over the same `TPriority` have inconsistent orderings, which is the kind of hidden-semantic coupling Bezel's single-order rule exists to prevent.

### D2 — Min-heap (smaller `CompareTo` = higher priority)

**Decision.** The element with the **smallest** priority (most-negative `CompareTo` result) is at the head and is dequeued first.

Rationale: `IComparable<T>.CompareTo` returns negative when `a < b`. Treating the *least* as the highest priority aligns with both the integer-deadline use case (next deadline = smallest timestamp = highest priority) and the mathematical convention for priority queues. A max-heap is obtained by wrapping the priority in a negating record.

### D3 — Unstable: equal priorities give no order guarantee

**Decision.** For two elements with equal priority (both yield `CompareTo = 0`), the dequeue order is **unspecified**.

*Why not stable.* A stable heap (FIFO within equal priority) requires tracking insertion sequence in each node, adding O(1) space per element. Most applications either don't care about equal-priority order or use a secondary priority to break ties (encoded in the `TPriority` type or a composite record). The cost — extra memory, extra comparison — is not justified by the benefit for the general case. Record the policy explicitly so callers don't assume stability.

### D4 — Try-convention shapes follow v0.171 (stdlib §1)

**Decision.** The Try methods use the v0.171 shape (a): `bool` + nullable `out` parameters.

For `TryDequeue`, both the element and the priority are relevant to the caller. Both are `out` parameters; both are nullable on the false path (nothing was dequeued):

```
TryDequeue(out element: TElement?, out priority: TPriority?): bool
TryPeek(out element: TElement?, out priority: TPriority?): bool
```

This is the only shape consistent with §22.3: every out must be assigned on every exit. On the false path, `null` is assigned to both.

### D5 — Iteration: heap order, NOT priority order; contrast with SortedDictionary

**Decision.** `PriorityQueue<TElement, TPriority>` implements `IReadOnlyCollection<PriorityQueueEntry<TElement, TPriority>>`, giving `Count` and unordered enumeration. Iteration over the queue **does not** visit elements in priority order — it visits them in heap-storage order, which is implementation-defined. This is explicitly unlike `SortedDictionary<K, V>` (§4.17), which guarantees **ascending key order** in its documentation: *"iteration visits entries in ascending key order."* `PriorityQueue` makes no such guarantee.

*Why expose iteration at all.* Iteration is useful for debugging and for snapshot/serialization uses (visit all elements to reconstruct the queue). Promising no ordering frees the implementation to expose internal storage order (O(n) iteration with no extra copy).

### D6 — Auxiliary entry-type record

**Decision.** A `PriorityQueueEntry<TElement, TPriority>` record is introduced as the **iteration element type** (`IReadOnlyCollection<PriorityQueueEntry<…>>`, §D5). It carries both the element and its priority, matching the caller's natural need when iterating. It is **not** the return type of `TryPeek`/`TryDequeue`: those follow the v0.171 two-nullable-out shape (§D4, `out element`/`out priority`), which is the standing Try convention for compound extraction — so `PriorityQueueEntry` serves iteration alone, not the Try surface.

### D7 — No DequeueEnqueue combined op in v1

**Decision.** `DequeueEnqueue(element: TElement, priority: TPriority): TElement` — which atomically removes the minimum and inserts a new element in O(log n) — is deferred. It is a useful but strictly additive operation; v1 ships without it, and it is recorded here so the method name and shape are not designed inconsistently elsewhere.

---

## 3. Proposed stdlib addition (ready-to-land text)

### New §4.19 — `PriorityQueue<TElement, TPriority>`

*(Numbered §4.19 after §4.18 `SortedSet<T>`; adjust at integration time if §4.18 is displaced.)*

```bezel
// Auxiliary entry record. Auto-hashable (§9.2 positional record).
public record PriorityQueueEntry<TElement, TPriority>(
    Element: TElement,
    Priority: TPriority
);

// Min-heap: the element with the smallest priority (lowest CompareTo return) is
// dequeued first.  Equal-priority order is UNSPECIFIED (unstable heap).
// Borrowing form: caller retains ownership of class-typed elements.
// The owning form (where TElement: class) is deferred to the #1334 two-axis review.
public class PriorityQueue<TElement, TPriority>:
    IReadOnlyCollection<PriorityQueueEntry<TElement, TPriority>>
    where TPriority: IComparable<TPriority>
  public constructor();
  public constructor(initialCapacity: int);  // ArgumentOutOfRangeException if capacity < 0

  public readonly property Count: int;

  // Insert an element with the given priority.  O(log n).
  public method Enqueue(element: TElement, priority: TPriority);

  // Remove and return the element with the smallest priority.  O(log n).
  // InvalidOperationException if empty.
  public method Dequeue(): TElement;

  // Return the element with the smallest priority without removing it.
  // Produces a borrow for class-typed TElement.  O(1).
  // InvalidOperationException if empty.
  public method Peek(): TElement;

  // Non-throwing forms (v0.171 shape-a: bool + nullable out).
  // Out parameters are null on the false (empty) path.
  public method TryDequeue(out element: TElement?, out priority: TPriority?): bool;
  public method TryPeek(out element: TElement?, out priority: TPriority?): bool;

  // Iteration (IReadOnlyCollection<PriorityQueueEntry<TElement, TPriority>>).
  // Order is heap-storage order — NOT priority order.
  // See §4.17 contrast: SortedDictionary iterates in ascending key order;
  // PriorityQueue does not.
  // Structural modification during iteration raises InvalidOperationException.

  // Remove all elements.  O(n).
  public method Clear();
end PriorityQueue;
```

**Ordering rule.** `TPriority.CompareTo` determines priority: `CompareTo` returning a *negative* number means "higher priority" (dequeued sooner). `Dequeue()` always returns the element whose priority has the smallest `CompareTo` value relative to all others.

**Stability.** Elements with equal priority — where `p1.CompareTo(p2) = 0` — may be dequeued in any order. Programs that require a specific order among equal-priority elements must encode a tie-breaker as part of `TPriority` (e.g., a composite record `(priority: int, insertionSeq: long)` with `CompareTo` comparing priority first, then sequence).

**Iteration order.** `for var e in queue do ... end` visits all `PriorityQueueEntry<TElement, TPriority>` elements in heap-storage order, which is unspecified and not priority order. Dequeue is the only operation that retrieves elements in priority order. This contrasts with `SortedDictionary<K, V>` (§4.17), whose iteration visits entries in ascending key order.

**Capacity.** The default constructor allocates a small initial storage; the capacity constructor pre-allocates `initialCapacity` slots. Neither affects observable behavior (only allocation strategy). The heap grows automatically on `Enqueue` when the backing store is full.

---

## 4. Usage example

```bezel
// Dijkstra-style nearest-node processing.
public method FindShortestPath(
    graph: Graph, start: int, target: int): Array<int>
  using var frontier := new PriorityQueue<int, int>();
  frontier.Enqueue(start, 0);
  while frontier.Count > 0 do
    var node := frontier.Dequeue();     // dequeues lowest-cost node
    if node = target then return ReconstructPath(node); end;
    for var neighbor in graph.Neighbors(node) do
      var cost := graph.Cost(node, neighbor);
      frontier.Enqueue(neighbor, cost);
    end;
  end;
  return [];        // no path
end FindShortestPath;
```

---

## 5. Boundary with the owning form (deferred)

`OwningPriorityQueue<TElement, TPriority> where TElement: class` would own its elements (taking `owned` on `Enqueue`, returning `owned` from `Dequeue`, disposing elements on `Clear`). This is explicitly blocked on the #1334 two-axis review, which governs the owning-collection naming and semantics. This proposal does not speculate on that shape; it is recorded as out-of-scope, not as declined.

---

## 6. Declined alternatives (register)

1. **Separate comparer constructor** (`new PriorityQueue<TElement, MyPriority>(new MyComparer())`) — the single-order rule of §4.17 applies; ordering lives on the type. Declined. (§D1)
2. **Stable ordering among equal-priority elements** — adds O(1) space per element and an extra comparison per sift; unjustified without a concrete consumer. Unstable and well-documented is the right default. (§D3)
3. **Max-heap constructor flag** — a negating-wrapper record is a local, testable fix for each call site; a global constructor flag would silently invert semantics and cannot be type-checked. Declined. (§D2)
4. **`DequeueEnqueue` combined op in v1** — additive, strictly deferred. (§D7)
5. **`PriorityQueue<T>` single-parameter shorthand (element = priority)** — that is a sorted multiset; `SortedSet<T>` (§4.18) already provides that for unique elements, and a borrowing sorted multiset is a distinct design. Not the right scope for this proposal.
