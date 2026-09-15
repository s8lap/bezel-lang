//! case: s28_10_0002
//! doc: spec
//! section: 28.10
//! title: a using var established inside a try's protected block is disposed before the try's own finally
//! rule: A try statement's own finally is established at entry to the
//!   protected block; an owned local or using var declared inside the
//!   protected block, established later, is disposed before the finally
//!   runs, in reverse establishment order.
//! cites: 28.10
//! expect: run
//! output: working
//!   dispose inner
//!   finally
module Conf;

public class Widget
  private _name: string;

  public constructor(name: string)
    _name := name;
  end constructor;

  destructor
    Console.WriteLine($"dispose {_name}");
  end destructor;
end Widget;

public class Program
  public static method Main()
    try
      using var w := new Widget("inner");
      Console.WriteLine("working");
    finally
      Console.WriteLine("finally");
    end;
  end Main;
end Program;
