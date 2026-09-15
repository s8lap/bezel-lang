//! case: s28_3_0044
//! doc: spec
//! section: 28.3
//! title: a binding atom disposes its owned temporary at the atom's completion
//! rule: A binding atom's clause whose initializer is an owned production is a named
//!   unbound owned temporary bounded by the atom, disposed at the atom's completion
//!   if undischarged.
//! cites: 17.15, C.3
//! expect: run
//! output: disposed
//!   7
module Conf;

public class Widget
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("disposed");
  end destructor;

  public method Value(): int => 7;
end Widget;

public class Program
  public static method Main()
    var n := (var w := new Widget(); w.Value());
    Console.WriteLine($"{n}");
  end Main;
end Program;
