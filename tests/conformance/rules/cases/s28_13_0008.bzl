//! case: s28_13_0008
//! doc: spec
//! section: 28.13
//! title: marking the transfer with move discharges a Weak<T> handle like
//!   any owned binding
//! rule: Weak handles are themselves ordinary owned class instances,
//!   discharged by `move` like any other owned binding (§28.3).
//! cites: 28.13, 28.3
//! expect: run
//! stdout: stored
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public class Sink
  public static method Store(owned w: Weak<Connection>): string
    using var held := move w;
    return "stored";
  end Store;
end Sink;

public class Program
  public static method Main()
    using var primary := new Shared<Connection>(new Connection());
    var weak := primary.Downgrade();
    Console.WriteLine(Sink.Store(move weak));
  end Main;
end Program;
