//! case: s27_5_8122
//! doc: spec
//! section: 27.5
//! title: a generic method's chain of three closure locals returns the captured parameter
//!  at T = string
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 27.5, 14.1
//! expect: run
//! output: hix
//!   hix
//! notes: Pins #9032: three nested closure-local calls in a generic body at T = string,
//!  instantiated from Run twice; each returns the captured parameter "hix".
module Conf;
public class Program
  public static method Use<T>(p: T): T
    var f := lambda() => p;
    var g := lambda() => f();
    var h := lambda() => g();
    var r := h();
    return r;
  end Use;
  public static method Run(): int
    var s := "hi" + "x";
    var r := Program.Use<string>(s);
    Console.WriteLine(r);
    var s2 := "hi" + "x";
    var r2 := Program.Use<string>(s2);
    Console.WriteLine(r2);
    return 0;
  end Run;
  public static method Main(): int
    Program.Run();
    return 0;
  end Main;
end Program;
