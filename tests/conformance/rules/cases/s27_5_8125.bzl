//! case: s27_5_8125
//! doc: spec
//! section: 27.5
//! title: a generic method calling a closure local after a nested call chain, at T = string
//!  and T = int
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 27.5, 14.1
//! expect: run
//! output: 5
//! notes: Pins #9032: a closure local that calls a generic method is called through another
//!  closure local; Use<string>("hix") has length 3 and Use<int>(2) is 2: 5.
module Conf;
public class Program
  public static method Id<T>(p: T): T return p; end Id;
  public static method Use<T>(p: T): T
    var f := lambda() => Program.Id<T>(p);
    var g := lambda() => f();
    return Program.Id<T>(g());
  end Use;
  public static method Run(): int
    var s := "hi" + "x";
    var t := Program.Use<string>(s).Length + Program.Use<int>(2);
    Console.WriteLine(t);
    return 0;
  end Run;
  public static method Main(): int
    Program.Run();
    return 0;
  end Main;
end Program;
