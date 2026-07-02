// Internal entry type.
private class Entry<K, V> where V: class
  public Key: K;
  public owned Value: V?;          // nullable for move-and-null restoration;
                                    //   non-null in committed dictionary state
  public owned Next: Entry<K, V>?;  // chain link
  public constructor(key: K, owned value: V, owned next: Entry<K, V>?)
    Key := key; Value := value; Next := next;
  end constructor;
end Entry;

public class OwningDictionary<K, V>: IDisposable where V: class
  private owned _buckets: OwningBuffer<Entry<K, V>>;
  private _count: int := 0;
  private _comparer: IEqualityComparer<K>;

  // ... constructors, BucketIndex, Count, Keys, Values, etc.

  public method Add(key: K, owned value: V)
    var idx := BucketIndex(key);
    // Drain the bucket's chain so we can walk it as owned local state
    // (Bezel's borrow tracker rejects walks-with-mutation; see note below).
    // Reverse the chain as we walk; if we don't find the key, the new entry
    // is prepended to the reversed chain — chain reversal is unobservable
    // because OwningDictionary doesn't promise insertion order.
    var chain := move _buckets[idx];
    _buckets[idx] := null;
    var rebuilt: Entry<K, V>? := null;
    var foundDup := false;
    while chain <> null do
      var rest := move chain.Next;
      chain.Next := rebuilt;          // restore (with the rebuilt-chain head)
      rebuilt := chain;                // ownership transfers in
      if not foundDup and _comparer.Equals(chain.Key, key) then
        foundDup := true;
      end;
      chain := rest;
    end;
    if foundDup then
      _buckets[idx] := rebuilt;       // restore bucket with unchanged chain
      throw new ArgumentException("duplicate key");
      // The owned `value` parameter is auto-disposed on throw per the
      // [§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows) exception-path discharge rule.
    end;
    _buckets[idx] := new Entry<K, V>(key, value, rebuilt);
    _count := _count + 1;
    MaybeGrow();
  end Add;

  public method PopValue(key: K): owned V
    var idx := BucketIndex(key);
    var chain := move _buckets[idx];
    _buckets[idx] := null;
    var rebuilt: Entry<K, V>? := null;
    var taken: V? := null;
    while chain <> null do
      var rest := move chain.Next;
      if taken = null and _comparer.Equals(chain.Key, key) then
        // Target: take its Value, dispose the entry node.
        taken := move chain.Value;
        chain.Value := null;             // restore (with null)
        chain.Next := null;              // restore (with null)
        chain.Dispose();                  // value and next are both null;
                                          //   destructor's auto-dispose passes are no-ops
      else
        chain.Next := rebuilt;            // restore (with rebuilt-chain head)
        rebuilt := chain;
      end;
      chain := rest;
    end;
    _buckets[idx] := rebuilt;             // restore bucket
    if taken = null then
      throw new KeyNotFoundException();
    end;
    _count := _count - 1;
    return taken!;
  end PopValue;

  // The rest of the implementation (TryGetValue with read-only chain walk,
  // ContainsKey, indexer get/set, GetOrAdd, Clear, MaybeGrow which calls Grow
  // when the load factor is exceeded, Grow which allocates a new larger
  // _buckets and rehashes every entry, Keys, Values, etc.) follows the same
  // patterns as Add and PopValue: every bucket mutation is a drain-and-rebuild.

  // No destructor declared; the compiler generates the default Dispose,
  // which cascades through _buckets → each Entry → each Entry's owned
  // Next and Value, recursively disposing the whole map.
end OwningDictionary;
