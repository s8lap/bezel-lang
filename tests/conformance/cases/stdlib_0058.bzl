public class OwningDictionary<K, V>: IDisposable where V: class
  public constructor();
  public constructor(capacity: int);
  public constructor(comparer: IEqualityComparer<K>);
  public constructor(capacity: int, comparer: IEqualityComparer<K>);

  public pure readonly property Count: int;
  public readonly property Keys: IReadOnlyCollection<K>;       // borrowing view
  public readonly property Values: IReadOnlyCollection<V>;     // borrowing view

  public property [key: K]: V
    get;                                       // borrow of stored value
    set(owned);                                // owned-taking; disposes previous
  end;

  public method Add(key: K, owned value: V);   // ArgumentException if the key is present; the passed
                                                //   value was consumed at the call site and is disposed
                                                //   on that path (v0.195) — pre-check ContainsKey when
                                                //   the caller must keep it
  public method ContainsKey(key: K): bool;
  public method Contains(key: K): bool;
  public method Remove(key: K): bool;          // disposes the removed value
  public method TryGetValue(key: K, out value: V?): bool;      // out is borrow; v0.171 (#1336)
  public method GetOrNull(key: K): V?;                         // v0.231 (§1 *OrNull tier): absent -> null;
                                                               //   present -> a borrow of the stored value, the
                                                               //   indexer's get posture — ownership stays with
                                                               //   the dictionary: a look, not a take (the take
                                                               //   forms remain PopValue/TryPopValue). where V:
                                                               //   class makes V statically non-nullable at
                                                               //   every instantiation (spec §7.10), so null
                                                               //   means absent, unambiguously. The §4.8 ??
                                                               //   limit (spec §7.7) applies at every
                                                               //   instantiation here: an owned fallback never
                                                               //   meets this borrow
  public method GetOrAdd(key: K, factory: (K) -> owned V): V;  // returns borrow

  // Drain
  public method PopValue(key: K): owned V;     // raises if not present. The `Value` suffix carries
                                               //   information and stays through the v0.259 egress
                                               //   rename (§4.10): the dictionary's egress transfers
                                               //   the *value* and consumes the *key* as a query, so
                                               //   `PopValue(key)` says which of the two the caller
                                               //   gets, where a bare `Pop(key)` would not — a
                                               //   naming-carries-information ground, not an
                                               //   exception to the rename
  public method TryPopValue(key: K, out owned value: V?): bool;   // v0.171 (#1336)
  public method Drain(): IOwningEnumerable<DrainedEntry<K, V>>;
                                               // iteration drains the dictionary;
                                               //   each yielded entry owns its Value

  public method Clear();                        // disposes every value
end OwningDictionary;

public class DrainedEntry<K, V>: IDisposable where V: class
  public constructor(key: K, owned value: V);  // ownership of value transfers in
  public readonly property Key: K;             // borrow / value copy of the key
  public owned Value: V;                        // owned — DrainedEntry is a class,
                                                //   so it may own its Value field
end DrainedEntry;
