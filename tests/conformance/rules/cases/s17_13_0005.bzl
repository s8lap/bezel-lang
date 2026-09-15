//! case: s17_13_0005
//! doc: spec
//! section: 17.13
//! title: an owned discard result is disposed at the end of the discard statement
//! rule: "An owned result is disposed at the end of the discard statement...
//!   `_ := expression;` evaluates `expression` and discharges the disposal
//!   obligation once the statement completes."
//! cites: 17.13, 28.3
//! expect: run
//! output: disposed
//!   next
module Conf;

public class Widget
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("disposed");
  end destructor;
end Widget;

public class Program
  public static method Main()
    _ := new Widget();
    Console.WriteLine("next");
  end Main;
end Program;
