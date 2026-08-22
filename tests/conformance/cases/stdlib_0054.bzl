public class Dictionary<K, V>: ICollection<DictionaryEntry<K, V>>
  public constructor();
  public constructor(capacity: int);
  public constructor(comparer: IEqualityComparer<K>);
  public constructor(capacity: int, comparer: IEqualityComparer<K>);

  public pure readonly property Count: int;
  public readonly property Keys: IReadOnlyCollection<K>;      // v0.165 (#1354): read-only views, matching §4.9
  public readonly property Values: IReadOnlyCollection<V>;

  public property [key: K]: V;

  public method Add(key: K, value: V);
  public method ContainsKey(key: K): bool;
  public method Contains(key: K): bool;            // alias for ContainsKey;
                                                   //   enables `key in dict`
  public method Remove(key: K): bool;
  public method TryGetValue(key: K, out value: V?): bool;   // v0.171 (#1336): false path assigns null; value-typed V reads as V? at the call site
  public method GetOrNull(key: K): V?;             // v0.231 (§1 *OrNull tier): absent -> null; present -> the
                                                   //   value — read semantics and caveat in the prose below
  public method GetOrAdd(key: K, factory: (K) -> V): V;

  public method Clear();                            // drops every entry; disposes neither
                                                    //   keys and values
end Dictionary;

public record DictionaryEntry<K, V>(Key: K, Value: V);

public interface IEqualityComparer<T>
  method Equals(a: T, b: T): bool;
  method GetHashCode(value: T): int;
end IEqualityComparer;
