module Cache.Ttl;

public interface IClock
  readonly property Now: int;
end IClock;

public class FakeClock: IClock
  private _now: int := 0;
  public property Now: int => _now;
  public method Advance(to: int) => _now := to;
end FakeClock;

public record Entry<V>(Value: V, Expires: int);

public class TtlCache<K, V> where K: hashable
  private _clock: IClock;
  private _ttl: int;
  private owned _entries: Dictionary<K, Entry<V>> := new Dictionary<K, Entry<V>>();

  public constructor(clock: IClock, ttl: int)
    _clock := clock;
    _ttl := ttl;
  end constructor;

  public method Put(key: K, value: V)
    _entries[key] := new Entry<V>(value, _clock.Now + _ttl);
  end Put;

  public method GetOrNull(key: K): V?
    // r42: GetOrNull over a generic-record value inside a generic class emits malformed IR (new)
    if not _entries.ContainsKey(key) then
      return null;
    end;
    var e := _entries[key];
    if e.Expires <= _clock.Now then
      return null;
    end;
    return e.Value;
  end GetOrNull;

  public method Purge(): int
    // r42: at K = string, keys projected with Select(kv.Key).ToArray() inside this generic class do not
    // match on Remove (silent; printing one segfaults) — new. Collect them with a loop instead.
    var dead := new Array<K>();
    for var kv in _entries do
      if kv.Value.Expires <= _clock.Now then dead.Add(kv.Key); end;
    end;
    for var k in dead do
      _ := _entries.Remove(k);
    end;
    return _entries.Count;
  end Purge;
end TtlCache;

public class Program
  public static method Main()
    using var clock := new FakeClock();
    using var cache := new TtlCache<string, int>(clock, 10);
    cache.Put("a", 1);
    clock.Advance(5);
    cache.Put("b", 2);
    clock.Advance(8);
    Console.WriteLine($"get a@8 -> {cache.GetOrNull("a")?.ToString() ?? "miss"}");
    clock.Advance(12);
    Console.WriteLine($"get a@12 -> {cache.GetOrNull("a")?.ToString() ?? "miss"}");
    Console.WriteLine($"get b@12 -> {cache.GetOrNull("b")?.ToString() ?? "miss"}");
    Console.WriteLine($"live {cache.Purge()}");
  end Main;
end Program;
