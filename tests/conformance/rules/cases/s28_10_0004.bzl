//! case: s28_10_0004
//! doc: spec
//! section: 28.10
//! title: the anonymous using var _ adopts a moved named owned binding exactly as the named form would
//! rule: Adoption composes — `using var _ := move x;` consumes `x` into the
//!   anonymous binding exactly as the named form would.
//! cites: 28.10
//! expect: run
//! output: adopted
//!   dispose temp
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
    var w := new Widget("temp");
    using var _ := move w;
    Console.WriteLine("adopted");
  end Main;
end Program;
