//! case: s28_7_0001
//! doc: spec
//! section: 28.7
//! title: reassigning an owned field evaluates the RHS before disposing the displaced value
//! rule: Assignment to an owned field disposes the displaced value; the
//!   right-hand side is evaluated first, then the previous live value is
//!   disposed, then the new value is stored.
//! cites: 28.7
//! expect: run
//! output: construct second
//!   dispose first
module Conf;

public class Widget
  private _name: string;

  public constructor(name: string)
    _name := name;
    Console.WriteLine($"construct {name}");
  end constructor;

  destructor
    Console.WriteLine($"dispose {_name}");
  end destructor;
end Widget;

public class Holder
  private owned _w: Widget;

  public constructor()
    _w := new Widget("first");
  end constructor;

  public method Replace()
    _w := new Widget("second");
  end Replace;
end Holder;

public class Program
  public static method Main()
    using var h := new Holder();
    h.Replace();
  end Main;
end Program;
