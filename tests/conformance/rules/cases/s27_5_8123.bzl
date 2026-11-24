//! case: s27_5_8123
//! doc: spec
//! section: 27.5
//! title: a generic method's returned closure calling closure locals yields the captured
//!  int
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 27.5, 14.1
//! expect: run
//! output: 7
//!   7
//! notes: Pins #9032: the returned closure calls g, which calls f, which reads the captured
//!  parameter 7; both the bound and the immediately-called spelling print 7.
module Conf;
public class Program
  public static method Use<T>(p: T): () -> T
    var f := lambda() => p;
    var g := lambda() => f();
    return lambda() => g();
  end Use;
  public static method Run(): int
    var s := 7;
    var h := Program.Use<int>(s);
    var r := h();
    Console.WriteLine(r);
    var s2 := 7;
    var r2 := Program.Use<int>(s2)();
    Console.WriteLine(r2);
    return 0;
  end Run;
  public static method Main(): int
    Program.Run();
    return 0;
  end Main;
end Program;
