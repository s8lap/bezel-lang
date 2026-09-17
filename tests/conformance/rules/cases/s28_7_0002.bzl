//! case: s28_7_0002
//! doc: spec
//! section: 28.7
//! title: reassigning a plain-var owned local disposes the displaced value
//! rule: Assignment to an owned field or owned local disposes the displaced
//!   value; the scope of this rule covers `owned` fields and plain-`var`
//!   owned locals alike.
//! cites: 28.7
//! expect: run
//! output: dispose first
//!   dispose second
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
    var w := new Widget("first");
    w := new Widget("second");
    w.Dispose();
  end Main;
end Program;
