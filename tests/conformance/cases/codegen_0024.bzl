public class Dictionary<K, V>: ICollection<DictionaryEntry<K, V>>    // K must be hashable
  constructor();  (capacity: int);  (comparer: IEqualityComparer<K>);  (capacity, comparer);
  pure readonly property Count: int;
  readonly property Keys: IReadOnlyCollection<K>;   Values: IReadOnlyCollection<V>;   // live views
  property [key: K]: V;                             // get: KeyNotFoundException; set: add-or-update
  method Add(key: K, value: V);                     // ArgumentException if present
  method ContainsKey(key: K): bool;   Contains(key: K): bool;     // Contains enables `key in dict`
  method Remove(key: K): bool;
  method TryGetValue(key: K, out value: V?): bool;
  method GetOrNull(key: K): V?;
  method GetOrAdd(key: K, factory: (K) -> V): V;    // a READ path — mutating its result mutates nothing
  method Clear();
end
public record DictionaryEntry<K, V>(Key: K, Value: V);
public interface IEqualityComparer<T>  method Equals(a: T, b: T): bool;  method GetHashCode(value: T): int;  end

public class OwningDictionary<K, V>: IDisposable where V: class
  constructor();  (capacity);  (comparer);  (capacity, comparer);
  pure readonly property Count: int;   readonly property Keys, Values;
  property [key: K]: V { get; set(owned); }         // get = borrow; set disposes previous
  method Add(key: K, owned value: V);               // if present: ArgumentException AND the value is disposed
  method ContainsKey(key)/Contains(key): bool;   method Remove(key: K): bool;   // Remove disposes
  method TryGetValue(key: K, out value: V?): bool;  // the out is a BORROW
  method GetOrNull(key: K): V?;                      // a look, not a take
  method GetOrAdd(key: K, factory: (K) -> owned V): V;      // returns a borrow
  method PopValue(key: K): owned V;   TryPopValue(key: K, out owned value: V?): bool;
  method Drain(): IOwningEnumerable<DrainedEntry<K, V>>;   method Clear();
end
public class DrainedEntry<K, V>: IDisposable where V: class
  constructor(key: K, owned value: V);   readonly property Key: K;   public owned Value: V;
end
