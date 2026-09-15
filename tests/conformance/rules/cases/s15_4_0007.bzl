//! case: s15_4_0007
//! doc: spec
//! section: 15.4
//! title: binding the source first remedies the fresh-element literal rejection
//! rule: The remedy for a fresh non-trivial-dispose literal element is to
//!   name the source first: `using var c := new Connection("a"); var conns:
//!   Array<Connection> := [c];` (§15.4).
//! cites: 15.4
//! expect: run
//! stdout: 1
module Conf;

public class Connection
  public constructor()
  end constructor;

  destructor
  end destructor;
end Connection;

public class Program
  public static method Main()
    using var c := new Connection();
    var conns: Array<Connection> := [c];
    Console.WriteLine(conns.Count);
  end Main;
end Program;
