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

// Borrowing key-iteration, one value taken at a time:
public method DrainAndProcess(dict: OwnedDictionary<string, Connection>)
  using var keys := new Array<string>(dict.Count);
  for var key in dict.Keys do
    keys.Add(key);
  end;
  for var key in keys do
    using var conn := dict.PopValue(key);     // takes ownership
    Process(conn);
  end;
  // dict is now empty
end DrainAndProcess;

// Single-call drain — each entry owns its value:
public method DrainAndProcess2(dict: OwnedDictionary<string, Connection>)
  for var entry in dict.Drain() do
    using var e := entry;                      // entry is owned; bind to discharge
    Process(e.Key, e.Value);                   // e.Value is the owned Connection
  end;
  // dict is now empty; each entry (and its Value) disposed at loop-body end
end DrainAndProcess2;
