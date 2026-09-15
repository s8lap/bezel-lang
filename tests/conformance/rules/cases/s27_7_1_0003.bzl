//! case: s27_7_1_0003
//! doc: spec
//! section: 27.7.1
//! title: a lambda never equals a method reference
//! rule: A lambda never equals a method reference.
//! cites: 27.7.1
//! expect: run
//! stdout: False
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Greet(): string => "hi";
end Widget;

public class Program
  public static method Main()
    var w := new Widget();
    var ref: () -> string := w.Greet;
    var lit: () -> string := lambda() => "hi";
    Console.WriteLine($"{ref = lit}");
  end Main;
end Program;
