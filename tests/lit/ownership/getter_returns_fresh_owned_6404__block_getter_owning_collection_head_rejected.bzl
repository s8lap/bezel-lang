// fix #6404 / §28.6 + §28.3 (spec v0.338): the §28.6 bullet-2 mint-announcing
// carve is read off the getter's DECLARED RETURN TYPE, and the set it admits is
// a stated UNION — `Task` ∪ iterable. §28.3's announcement table ROW 2 states it
// normatively: "a `Task`-family type (`Task`, `Task<T>`, `OwningTask<T>`) or an
// iterable-family type: one of the eight §29.3/§30.7 iterable/enumerator
// interface heads, or a lazy query". A concrete owning collection is ROW 4, an
// ordinary borrowed reference, and §28.6's own worked example rejects a fresh
// one through a non-`owned` return ("LeakTransactions"). So the carve stops at
// `OwningArray<Item>`.
//
// THE BODY IS BYTE-IDENTICAL to the ACCEPT lit/gtest rows at
// `IOwningEnumerable<Item>` and `IEnumerable<Item>` — same `new
// OwningArray<Item>()` producer, same local, same single hop. Only the DECLARED
// RETURN TYPE differs. A carve keyed on the producer expression's type — the
// `OwningArray<Item>` all three programs share — would accept this one too, and
// one local would then decide the verdict.
// per §28.6, §28.3 (announcement table rows 2 and 4), K6.2, #6404

// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class Item public V: int; public constructor(v: int) V := v; end constructor; end Item; class C public property Items: OwningArray<Item> get => do var a := new OwningArray<Item>(); a.Add(new Item(5)); return a; end; end Items; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a property or indexer getter may not return a freshly-produced owned value — a getter is a borrow-producing position (properties and indexers do not carry `owned`), so the value would escape with no owner and leak; expose the factory as a method returning `owned T`
// CHECK-SAME: (§28.6)