//! case: s22_7_0011
//! doc: spec
//! section: 22.7
//! title: a static constructor does not suppress the implicit parameterless constructor
//! rule: A `static constructor` does not suppress the implicit parameterless
//!   constructor (it is never a chaining or `new` target); it runs once before
//!   the type's first use.
//! cites: 22.7, 22.13
//! expect: run
//! output: static
//!   made
//! issue: 4364
module Conf;

public class Widget
  public Name: string := "made";

  static constructor()
    Console.WriteLine("static");
  end constructor;
end Widget;

public class Program
  public static method Main()
    using var w := new Widget();
    Console.WriteLine(w.Name);
  end Main;
end Program;
