//! case: s28_6_0039
//! doc: spec
//! section: 28.6
//! title: a slot of a local owning inline array is disposed explicitly and the scope end disposes it once
//! rule: requires a receiver the disposer owns ... or a slot of an owning
//!   inline array held by an owned local or `owned` parameter of the body
//! cites: 28.6, 15.1.1, 15.3, 28.2
//! expect: run
//! output: dispose first
//!   released early
//!   dispose second
//!   after
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
    do
      var pool: [2 owned]Connection := [new Connection("first"), new Connection("second")];
      pool[0].Dispose();
      Console.WriteLine("released early");
    end;
    Console.WriteLine("after");
  end Main;
end Program;
