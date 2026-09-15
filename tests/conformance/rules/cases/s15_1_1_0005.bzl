//! case: s15_1_1_0005
//! doc: spec
//! section: 15.1.1
//! title: an owning inline array local disposes its slots in reverse slot order at scope end
//! rule: When an owning inline array's lifetime ends, the array's auto-dispose
//!   pass disposes each non-null slot **in reverse slot order**; for locals
//!   this happens at scope end via the standard owned-local discharge.
//! cites: 15.1.1, 15.3, 28.3
//! expect: run
//! output: first
//!   dispose second
//!   dispose first
//!   after
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
    do
      var pool: [2 owned]Connection := [new Connection("first"), new Connection("second")];
      Console.WriteLine(pool[0].Name);
    end;
    Console.WriteLine("after");
  end Main;
end Program;
