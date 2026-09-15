//! case: s15_2_0002
//! doc: spec
//! section: 15.2
//! title: an owning heap array constructed via literal owns its elements
//! rule: `OwningArray<T>` is a class type allocated with `new` and requiring
//!   disposal via `using var`; a collection literal transfers ownership of
//!   each element into it (§15.2).
//! cites: 15.2, 15.4
//! expect: run
//! stdout: 3
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public class Program
  public static method Main()
    using var pool: OwningArray<Connection> := [
      new Connection(), new Connection(), new Connection()
    ];
    Console.WriteLine(pool.Count);
  end Main;
end Program;
