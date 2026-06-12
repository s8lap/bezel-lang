public class OwnedDictionary<K, V>: IDisposable where V: class
  public constructor();
  public constructor(capacity: int);
  public constructor(comparer: IEqualityComparer<K>);
  public constructor(capacity: int, comparer: IEqualityComparer<K>);

  public readonly property Count: int;
  public readonly property Keys: IReadOnlyCollection<K>;       // borrowing view
  public readonly property Values: IReadOnlyCollection<V>;     // borrowing view

  public property [key: K]: V
    get;                                       // borrow of stored value
    set(owned);                                // owned-taking; disposes previous
  end;

  public method Add(key: K, owned value: V);
  public method ContainsKey(key: K): bool;
  public method Contains(key: K): bool;
  public method Remove(key: K): bool;          // disposes the removed value
  public method TryGetValue(key: K, out value: V): bool;       // out is borrow
  public method GetOrAdd(key: K, factory: (K) -> owned V): V;  // returns borrow

  // Drain
  public method PopValue(key: K): owned V;     // raises if not present
  public method TryPopValue(key: K, out owned value: V): bool;
  public method Drain(): OwnedEnumerable<DrainedEntry<K, V>>;
                                               // iteration drains the dictionary;
                                               //   each yielded entry owns its Value

  public method Clear();                        // disposes every value
end OwnedDictionary;

public class DrainedEntry<K, V>: IDisposable where V: class
  public constructor(key: K, owned value: V);  // ownership of value transfers in
  public readonly property Key: K;             // borrow / value copy of the key
  public owned Value: V;                        // owned — DrainedEntry is a class,
                                                //   so it may own its Value field
end DrainedEntry;

using var pool: OwnedDictionary<string, Connection> := [
  "primary": Connect("p"),
  "backup":  Connect("b")
];
