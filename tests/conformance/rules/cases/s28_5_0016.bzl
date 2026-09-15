//! case: s28_5_0016
//! doc: spec
//! section: 28.5
//! title: owned fields are disposed in reverse declaration order
//! rule: The `owned` modifier auto-disposes the field when the containing
//!   object is disposed, in reverse declaration order.
//! cites: 28.4, 28.5
//! expect: run
//! output: dispose second
//!   dispose first
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

public class Pair
  private owned _first: Widget;
  private owned _second: Widget;

  public constructor()
    _first := new Widget("first");
    _second := new Widget("second");
  end constructor;
end Pair;

public class Program
  public static method Main()
    using var p := new Pair();
  end Main;
end Program;
