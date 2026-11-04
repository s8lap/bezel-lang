//! case: s2_7_0806
//! doc: spec
//! section: 2.7
//! title: a lambda body calling WriteLine on a local named Console that returns int types the call as int
//! rule: If it resolves to a *value* (a local, field, or property), the chain is member access.
//! cites: 2.7, 2.3
//! expect: run
//! output: 42
//!   43
//! issue: 8126
//! notes: the built-in Console.WriteLine returns no value; here the user method returns int, 41 + Length.
module Conf;

public class Sh
  public method WriteLine(s: string): int
    return 41 + s.Length;
  end WriteLine;
end Sh;

public class Program
  public static method Show(x: int)
    Console.WriteLine(x);
  end Show;
  public static method Main()
    var Console := new Sh();
    var f := lambda() => Console.WriteLine("z");
    var g := lambda(): int => Console.WriteLine("zz");
    Show(f());
    Show(g());
  end Main;
end Program;
