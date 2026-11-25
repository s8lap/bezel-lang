//! case: s23_4_9503
//! doc: spec
//! section: 23.4
//! title: an indexer overloaded by an int and by two function types selects each by its argument
//! rule: the candidate is applicable at the position when the lambda's inferred return type equals the
//!   candidate's return slot, implicitly converts to it
//! cites: 23.4, 22.6
//! expect: run
//! output: 1
//!   2
//!   3
//! notes: Pins #9187 (pick 3715595a8): an indexer overloaded only by a function-typed parameter binds the
//!   overload whose function type matches the argument's (sema and codegen agreed on the
//!   first-declared overload before the fix). The int argument selects the int overload; the two
//!   lambdas select by return type.
module Conf;

public class C
  public property [i: int]: int => 1;
  public property [f: () -> int]: int => 2;
  public property [f: () -> string]: int => 3;
  public constructor()
  end constructor;
end C;

public class Program
  public static method Main(): int
    using var c := new C();
    Console.WriteLine(c[1]);
    Console.WriteLine(c[lambda() => 1]);
    Console.WriteLine(c[lambda() => "q"]);
    return 0;
  end Main;
end Program;
