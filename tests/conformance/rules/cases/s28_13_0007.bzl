//! case: s28_13_0007
//! doc: spec
//! section: 28.13
//! title: a Weak<T> handle is an ordinary owned binding requiring move at
//!   an owned-consuming call
//! rule: Weak handles are themselves ordinary owned class instances and are
//!   disposed like any other — including the call-site `move` marker
//!   required when a named owned binding feeds an `owned` argument (§28.3).
//! cites: 28.13, 28.3
//! expect: reject
//! diagnostic: move
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
    Console.WriteLine(Sink.Store(weak));
  end Main;
end Program;
