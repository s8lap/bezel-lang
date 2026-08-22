public class SortedDictionary<K, V>: IReadOnlyCollection<DictionaryEntry<K, V>> where K: IComparable<K>   // conformance added v0.165 (#1354): makes for-in/LINQ legal per spec §29; explicit IDisposable dropped v0.195 (§1 convention — implicit, trivial-dispose)
  public constructor();

  public pure readonly property Count: int;

  public property [key: K]: V;                  // get: KeyNotFoundException if absent
                                                // set: insert or replace
  public method ContainsKey(key: K): bool;
  public method Contains(key: K): bool;         // alias for ContainsKey; enables `key in dict` (§4.8 convention; v0.195)
  public method TryGetValue(key: K, out value: V?): bool;   // v0.171 (#1336): nullable out, matching §4.8's revised uniform Try shape
  public method GetOrNull(key: K): V?;          // v0.231 (§1 *OrNull tier): absent -> null; present -> the
                                                //   value under §4.8's read semantics — caveat in the prose
                                                //   below
  public method Remove(key: K): bool;           // false if absent
  public method Clear();

  public pure readonly property FirstKey: K;         // least key; InvalidOperationException if empty
  public pure readonly property LastKey: K;          // greatest key; InvalidOperationException if empty
end SortedDictionary;
