//! case: s22_6_0125
//! doc: spec
//! section: 22.6
//! title: a user class overloading LastIndexOf by parameter count selects each overload by the call arity
//! rule: Methods and constructors may be overloaded by parameter list
//! cites: 22.6, 26.5
//! expect: run
//! output: 3
//!   7
module Conf;

class Finder
  public method LastIndexOf(a: int, b: int): int => a + b;
  public method LastIndexOf(): int => 7;
end Finder;

public class Program
  public static method Main()
    var f := new Finder();
    Console.WriteLine(f.LastIndexOf(1, 2));
    Console.WriteLine(f.LastIndexOf());
  end Main;
end Program;
