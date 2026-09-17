//! case: s28_13_0001
//! doc: spec
//! section: 28.13
//! title: a Shared<T> handle is an ordinary owned binding requiring move at
//!   an owned-consuming call
//! rule: Handles are ordinary owned class instances. Every rule of this
//!   chapter applies to a handle unchanged — including the call-site
//!   `move` marker required when a named owned binding feeds an `owned`
//!   argument (§28.3).
//! cites: 28.13, 28.3
//! expect: reject
//! diagnostic: move
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
    Console.WriteLine(Sink.Absorb(primary));
  end Main;
end Program;
