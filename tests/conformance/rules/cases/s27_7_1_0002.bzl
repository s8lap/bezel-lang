//! case: s27_7_1_0002
//! doc: spec
//! section: 27.7.1
//! title: repeated evaluations of a method reference on the same receiver are guaranteed equal
//! rule: A method reference equals another method reference iff both denote
//!   the same method and, for instance references, are bound to the same
//!   receiver — repeated evaluations of `obj.Method` on the same class-typed
//!   `obj` are therefore guaranteed equal.
//! cites: 27.7.1
//! expect: run
//! stdout: True
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Greet(): string => "hi";
end Widget;

public class Program
  public static method Main()
    var w := new Widget();
    Console.WriteLine($"{w.Greet = w.Greet}");
  end Main;
end Program;
