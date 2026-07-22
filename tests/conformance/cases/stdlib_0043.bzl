public class Dictionary<K, V>: ICollection<DictionaryEntry<K, V>>
  public constructor();
  public constructor(capacity: int);
  public constructor(comparer: IEqualityComparer<K>);
  public constructor(capacity: int, comparer: IEqualityComparer<K>);

  public readonly property Count: int;
  public readonly property Keys: ICollection<K>;
  public readonly property Values: ICollection<V>;

  public property [key: K]: V;

  public method Add(key: K, value: V);
  public method ContainsKey(key: K): bool;
  public method Contains(key: K): bool;            // alias for ContainsKey;
                                                   //   enables `key in dict`
  public method Remove(key: K): bool;
  public method TryGetValue(key: K, out value: V): bool;
  public method GetOrAdd(key: K, factory: (K) -> V): V;

  public method Clear();
  // (§28.11 v0.213, #2081: `DisposeElements()` removed from Dictionary<K, V> with
  // axiom A8 — values a holder owns belong in OwningDictionary<K, V>, whose
  // Clear()/Dispose() cascades over the values.)
end Dictionary;

public record DictionaryEntry<K, V>(Key: K, Value: V);

public interface IEqualityComparer<T>
  method Equals(a: T, b: T): bool;
  method GetHashCode(value: T): int;
end IEqualityComparer;

using var ages: Dictionary<string, int> := [
  "alice": 30,
  "bob":   25
];
