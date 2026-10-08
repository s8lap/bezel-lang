public class SortedDictionary<K, V>: IReadOnlyCollection<DictionaryEntry<K, V>> where K: IComparable<K>
  constructor();   pure readonly property Count: int;
  property [key: K]: V;                          // get: KeyNotFoundException;  set: insert or replace
  method ContainsKey(key)/Contains(key): bool;   TryGetValue(key, out value: V?): bool;
  method GetOrNull(key: K): V?;   Remove(key: K): bool;   Clear();
  pure readonly property FirstKey: K;   LastKey: K;        // InvalidOperationException if empty
end
public class SortedSet<T>: IReadOnlyCollection<T> where T: IComparable<T>
  constructor();   pure readonly property Count: int;
  method Add(item: T);            // returns (); adding an equal element is a silent no-op
  method TryAdd(item: T): bool;  Contains(item: T): bool;  Remove(item: T): bool;  Clear();
  pure readonly property First: T;   Last: T;              // InvalidOperationException if empty
end
public class FrozenArray<T>: IEnumerable<T>      // deeply immutable, sendable; T: sendable value types and string
  public static method From(owned buffer: OwningBuffer<T>): owned FrozenArray<T>;   // VALUE T; O(1), steals
  public static method From(items: Array<T>): owned FrozenArray<T>;                 // copies; only form for string
  public readonly property Length: int;    public property [i: int]: T { get }
  public method ToArray(): owned Array<T>;    destructor;
end
public class Counter<K>: IEnumerable<DictionaryEntry<K, int>> where K: hashable   // NOT YET IMPLEMENTED (#3848)
  constructor();
  method Increment(key: K, delta: int := 1);     // absent key is created at `delta`; negative admitted
  method CountOf(key: K): int;                   // 0 when absent — NOT pure
  method ContainsKey(key: K): bool;              // distinguishes present-at-zero from absent
  method Remove(key: K): bool;   Clear();
  pure readonly property Count: int;             // distinct keys
  pure readonly property Total: int;             // sum of counts; may be negative
end
