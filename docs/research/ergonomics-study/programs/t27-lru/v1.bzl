module Generic.Lru;

public class LruCache<K, V> where K: hashable
  private _capacity: int;
  private owned _map: Dictionary<K, V> := new Dictionary<K, V>();
  private owned _order: LinkedList<K> := new LinkedList<K>();

  public constructor(capacity: int)
    _capacity := capacity;
  end constructor;

  public method Get(key: K): V?
    if not _map.ContainsKey(key) then
      return null;
    end;
    Touch(key);
    return _map[key];
  end Get;

  public method Put(key: K, value: V)
    if _map.ContainsKey(key) then
      Touch(key);
    else
      if _map.Count = _capacity then
        var oldest := _order.First!;
        _order.RemoveFirst();
        _map.Remove(oldest);
        Console.WriteLine($"evicted {oldest}");
      end;
      _order.AddLast(key);
    end;
    _map[key] := value;
  end Put;

  private method Touch(key: K)
    var node := _order.Find(key);
    if node <> null then
      _order.RemoveNode(node);
    end;
    _order.AddLast(key);
  end Touch;
end LruCache;

public class Program
  private static method Show(c: LruCache<string, int>, key: string)
    var v := c.Get(key);
    Console.WriteLine($"get {key} -> {v ?? "miss"}");
  end Show;

  public static method Main()
    using var c := new LruCache<string, int>(2);
    c.Put("a", 1);
    c.Put("b", 2);
    Show(c, "a");
    c.Put("c", 3);
    Show(c, "b");
    Show(c, "c");
    c.Put("a", 10);
    Show(c, "a");
  end Main;
end Program;
