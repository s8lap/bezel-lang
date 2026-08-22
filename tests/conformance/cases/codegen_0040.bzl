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

public class Customer(name: string)
  public readonly property Name: string := name;
end Customer;

public class Ledger(owner: Customer)
  private owned _entries: Array<string> := new Array<string>();
  private owned _cache: OwningArray<Connection>? := null;   // nullable owned field
  private _owner: Customer := owner;                        // back-reference — NOT owned
end Ledger;
