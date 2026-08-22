public class Counter<K>: IEnumerable<DictionaryEntry<K, int>> where K: hashable
  public constructor();

  public method Increment(key: K, delta: int := 1);   // absent key is created at `delta`
  public method CountOf(key: K): int;                 // 0 when absent — not `pure`: compares
                                                      //   via K's equality (§22.15's line;
                                                      //   the same reason Contains is not pure)
  public method ContainsKey(key: K): bool;            // distinguishes present-at-zero from absent
  public method Remove(key: K): bool;
  public method Clear();

  public pure readonly property Count: int;           // distinct keys currently stored
  public pure readonly property Total: int;           // sum of all counts (may be negative)
end Counter;
