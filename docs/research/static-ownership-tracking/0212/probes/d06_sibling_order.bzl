module Probe;
// DESTRUCTOR WINDOW, siblings: `_cache` borrows its sibling `_db`. Right order: the borrowing field is declared AFTER the field it borrows, so the cascade disposes it first and its destructor reads a live sibling. EXPECT: clean in both modes. (d07 is the wrong order.)
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
  private owned _db: Db;
  private owned _cache: Cache;
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
