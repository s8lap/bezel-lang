//! case: s28_3_0044
//! doc: spec
//! section: 28.3
//! title: a binding atom disposes its owned temporary at the atom's completion
//! rule: A binding atom's clause whose initializer is an owned production is a named
//!   unbound owned temporary bounded by the atom, disposed at the atom's
//!   completion, before the enclosing expression continues.
//! expect: run
//! stdout: disposed
module Conf;

public class Widget
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
