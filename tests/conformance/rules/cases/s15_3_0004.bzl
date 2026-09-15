//! case: s15_3_0004
//! doc: spec
//! section: 15.3
//! title: whole-value reassignment of an owning inline array disposes the displaced live slot
//! rule: Whole-value reassignment on a move-only local is drop-on-overwrite
//!   applied per slot: the RHS is evaluated first, then each live slot of the
//!   displaced value is disposed, then the new value is stored; the replacement
//!   is itself auto-disposed at scope end (§15.3, §15.1.1).
//! cites: 15.3
//! expect: run
//! output: dispose first
//!   done
//!   dispose second
module Conf;

public class Connection
  public property Name: string;

  public constructor(name: string)
    Name := name;
  end constructor;

  destructor
    Console.WriteLine($"dispose {Name}");
  end destructor;
end Connection;

public class Program
  public static method Main()
    var a: [1 owned]Connection := [new Connection("first")];
    a := [new Connection("second")];
    Console.WriteLine("done");
  end Main;
end Program;
