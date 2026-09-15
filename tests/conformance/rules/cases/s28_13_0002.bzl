//! case: s28_13_0002
//! doc: spec
//! section: 28.13
//! title: marking the transfer with move discharges a Shared<T> handle like
//!   any owned binding
//! rule: Handles are ordinary owned class instances, discharged by `move`
//!   like any other owned binding (§28.3).
//! cites: 28.13, 28.3
//! expect: run
//! stdout: conn
module Conf;

public class Connection
  public constructor()
  end constructor;

  public method Tag(): string => "conn";
end Connection;

public class Sink
  public static method Absorb(owned h: Shared<Connection>): string
    using var held := move h;
    return held.Get().Tag();
  end Absorb;
end Sink;

public class Program
  public static method Main()
    var primary := new Shared<Connection>(new Connection());
    Console.WriteLine(Sink.Absorb(move primary));
  end Main;
end Program;
