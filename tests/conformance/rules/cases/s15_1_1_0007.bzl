//! case: s15_1_1_0007
//! doc: spec
//! section: 15.1.1
//! title: whole-value reassignment disposes the displaced slots in reverse slot order
//! rule: A third ending is **whole-value reassignment**: overwriting an
//!   owning-inline-array local or instance field disposes each live slot of
//!   the displaced value before the store, in this section's reverse slot
//!   order — §15.3's per-slot drop-on-overwrite, the same pass run at the
//!   overwrite point.
//! cites: 15.1.1, 15.3, 28.7
//! expect: run
//! output: dispose a2
//!   dispose a1
//!   stored
//!   dispose b2
//!   dispose b1
//! issue: 4388
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
    var pool: [2 owned]Connection := [new Connection("a1"), new Connection("a2")];
    pool := [new Connection("b1"), new Connection("b2")];
    Console.WriteLine("stored");
  end Main;
end Program;
