public class Connection(host: string)
  public readonly property Host: string := host;
  public method SendShutdown()
    Console.WriteLine($"closing {Host}");
  end SendShutdown;
end Connection;

public static class Pooling
  public static method Connect(host: string): owned Connection     // static + class type => `owned` REQUIRED
    return new Connection(host);
  end Connect;

  public static method BuildPool(): owned OwningArray<Connection>
    var pool := new OwningArray<Connection>();
    pool.Add(Connect("a"));                      // fresh producer, unmarked; the slot takes ownership
    pool.Add(new Connection("b"));
    return pool;                                 // target-announced: NOT `return move pool;`
  end BuildPool;

  public static method Shutdown()
    using var pool := BuildPool();               // the caller owns it; disposal cascades to elements
    for var c in pool do                         // borrowing pass: `c` is a borrow of the slot
      Console.WriteLine(c.Host);
    end;
    for var c in pool.Drain() do                 // owning pass: each element leaves the collection
      using var live := move c;                  // adopt on the first line — covers every exit edge
      live.SendShutdown();
    end;                                         // pool is now empty, still disposable
  end Shutdown;
end Pooling;

import System.Collections.Generic;

public class ConnectionPool
  private owned _cache: OwningDictionary<string, Connection> := new OwningDictionary<string, Connection>();

  public method TryPopCached(name: string, out owned conn: Connection? when true): bool
    if not _cache.ContainsKey(name) then
      conn := null;                              // every out path must assign (throw paths are exempt)
      return false;
    end;
    conn := _cache.PopValue(name);               // takes ownership out of the cache
    return true;
  end TryPopCached;

  public method Drop(name: string)
    if TryPopCached(name, out using var c) then
      c.SendShutdown();
    end;                                         // c is disposed at the end of this block
  end Drop;
end ConnectionPool;
