//! case: s28_13_0006
//! doc: spec
//! section: 28.13
//! title: Get()'s borrow is fine to use within the sourcing handle's own
//!   scope
//! rule: `Get(): T` produces a borrow of the underlying value, valid while
//!   the handle lives; used within that scope it is an ordinary borrow.
//! cites: 28.13, 28.3
//! expect: run
//! stdout: conn
module Conf;

public class Connection
  public constructor()
  end constructor;

  public method Tag(): string => "conn";
end Connection;

public class Leaker
  public constructor()
  end constructor;

  public method Peek(): string
    using var h := new Shared<Connection>(new Connection());
    return h.Get().Tag();
  end Peek;
end Leaker;

public class Program
  public static method Main()
    using var l := new Leaker();
    Console.WriteLine(l.Peek());
  end Main;
end Program;
