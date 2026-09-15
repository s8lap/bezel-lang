//! case: s27_6_0003
//! doc: spec
//! section: 27.6
//! title: a bound instance method reference captures the receiver at formation
//! rule: Method references work for instance methods — the instance is
//!   captured at formation, by reference for class-typed receivers.
//! cites: 27.6
//! expect: run
//! stdout: Hello, World!
module Conf;

public class Greeter
  private _prefix: string;

  public constructor(prefix: string)
    _prefix := prefix;
  end constructor;

  public method Greet(name: string): string
    return $"{_prefix}, {name}!";
  end Greet;
end Greeter;

public class Program
  public static method Main()
    var g := new Greeter("Hello");
    var greet: (string) -> string := g.Greet;
    Console.WriteLine(greet("World"));
  end Main;
end Program;
