//! case: s27_5_8124
//! doc: spec
//! section: 27.5
//! title: a generic method's lambda-in-lambda call of a closure local at two instantiations
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 27.5, 14.1
//! expect: run
//! output: 7
//! notes: Pins #9032: a lambda inside a lambda calls the closure local f, at T = string
//!  (length 3) and T = int (value 4): 3 + 4 = 7.
module Conf;
public class Program
  public static method Use<T>(p: T): T
    var f := lambda() => p;
    var g := lambda() => (lambda() => f())();
    return g();
  end Use;
  public static method Run(): int
    var s := "hi" + "x";
    var t := 0;
    t := t + Program.Use<string>(s).Length;
    t := t + Program.Use<int>(4);
    Console.WriteLine(t);
    return 0;
  end Run;
  public static method Main(): int
    Program.Run();
    return 0;
  end Main;
end Program;
