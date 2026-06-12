public class SortedDictionary<K, V>: IDisposable where K: IComparable<K>
  public constructor();

  public readonly property Count: int;

  public property [key: K]: V;                  // get: KeyNotFoundException if absent
                                                // set: insert or replace
  public method ContainsKey(key: K): bool;
  public method TryGetValue(key: K, out value: V?): bool;
  public method Remove(key: K): bool;           // false if absent
  public method Clear();

  public readonly property FirstKey: K;         // least key; InvalidOperationException if empty
  public readonly property LastKey: K;          // greatest key; InvalidOperationException if empty
end SortedDictionary;
