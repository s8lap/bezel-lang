//! case: s15_4_0004
//! doc: spec
//! section: 15.4
//! title: a fresh collection literal returned through an owned return transfers to the caller
//! rule: A collection literal targeting a reference type produces an owned
//!   value that must reach an owned location; `return [...]` is admitted
//!   through an `owned` return, the container's obligation following the
//!   transfer (§15.4, Appendix C.1).
//! cites: 15.4, 28.3
//! expect: run
//! stdout: 2
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public static class Factory
  public static method Build(): owned OwningArray<Connection>
    return [new Connection(), new Connection()];
  end Build;
end Factory;

public class Program
  public static method Main()
    using var pool := Factory.Build();
    Console.WriteLine(pool.Count);
  end Main;
end Program;
