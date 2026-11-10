module Probe;
// DESTRUCTOR WINDOW, siblings, wrong order: `_cache` is declared BEFORE the `_db` it borrows, so the cascade disposes `_db` first and the cache's destructor reads a disposed sibling. EXPECT today: compiles; checked = DisposedAccessException. Under 0212: error at the store (the declared-before test on fields).
public class Db
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
  destructor
    Console.WriteLine("db closed");
  end destructor;
end Db;
public class Cache
  private _db: Db;
  public constructor(db: Db)
    _db := db;
  end constructor;
  destructor
    Console.WriteLine($"cache flushes to {_db.Name}");
  end destructor;
end Cache;
public class Root
  private owned _cache: Cache;
  private owned _db: Db;
  public constructor()
    _db := new Db("main");
    _cache := new Cache(_db);
  end constructor;
end Root;
public class Program
  public static method Main(): int
    using var r := new Root();
    Console.WriteLine("made");
    return 0;
  end Main;
end Program;
