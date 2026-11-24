//! case: s27_5_8121
//! doc: spec
//! section: 27.5
//! title: a generic method's lambda that calls a closure local, instantiated twice from a
//!  non-Main method
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 27.5, 14.1
//! expect: run
//! output: 2
//! notes: Pins #9032 (the issue's repro): the lambda g calls the closure local f in a
//!  generic body instantiated from Run, not from Main. Use returns 1 each call, so Run
//!  returns 2.
module Conf;
public class Program
  public static method Use<T>(p: T): int
    var f := lambda() => p;
    var g := lambda() => f();
    var r := g();
    return 1;
  end Use;
  public static method Run(): int
    var s := "hi" + "x";
    var t := 0;
    t := t + Program.Use<string>(s);
    t := t + Program.Use<string>(s);
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
