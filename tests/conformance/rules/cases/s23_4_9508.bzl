//! case: s23_4_9508
//! doc: spec
//! section: 23.4
//! title: indexers overloaded by int, string, bool and arity select by the argument list
//! rule: Indexers may be overloaded by parameter type list.
//! cites: 23.4, 22.6
//! expect: run
//! output: 1
//!   2
//!   3
//!   4
//! notes: Spec-decided control of the #9187 fix (pick 3715595a8): overloads that do not involve a function
//!   type select by the argument's type and by arity, before and after.
module Conf;

public class C
  public property [i: int]: int => 1;
  public property [s: string]: int => 2;
  public property [b: bool]: int => 3;
  public property [i: int, j: int]: int => 4;
  public constructor()
  end constructor;
end C;

public class Program
  public static method Main(): int
    using var c := new C();
    Console.WriteLine(c[1]);
    Console.WriteLine(c["a"]);
    Console.WriteLine(c[true]);
    Console.WriteLine(c[1, 2]);
    return 0;
  end Main;
end Program;
