//! case: s15_1_1_0006
//! doc: spec
//! section: 15.1.1
//! title: an owning-inline-array instance field disposes its slots in reverse slot order
//! rule: For **instance** fields of owning-inline-array type, the enclosing
//!   class's compiler-generated `Dispose()` disposes each non-null slot — in
//!   the same reverse slot order as every other ending of the value — and no
//!   user-written `destructor` is required.
//! cites: 15.1.1, 28.4, 28.5
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

public class Pool
  private _conns: [2 owned]Connection;

  public constructor()
    _conns := [new Connection("first"), new Connection("second")];
  end constructor;

  public method FirstName(): string => _conns[0].Name;
end Pool;

public class Program
  public static method Main()
    do
      using var p := new Pool();
      Console.WriteLine(p.FirstName());
    end;
    Console.WriteLine("after");
  end Main;
end Program;
