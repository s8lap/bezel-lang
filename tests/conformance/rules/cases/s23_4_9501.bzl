//! case: s23_4_9501
//! doc: spec
//! section: 23.4
//! title: an indexer overloaded by function type selects the overload the lambda or function value matches
//! rule: the candidate is applicable at the position when the lambda's inferred return type equals the
//!   candidate's return slot, implicitly converts to it
//! cites: 23.4, 22.6, 27.1
//! expect: run
//! output: 2
//!   1
//!   3
//!   2
//!   1
//!   3
//! notes: Pins #9187 (pick 3715595a8): an indexer overloaded only by a function-typed parameter binds the
//!   overload whose function type matches the argument's (sema and codegen agreed on the
//!   first-declared overload before the fix). `() => 7` is exact at the `() -> int` slot (2) and
//!   inapplicable at the string slot and by parameter count at the (int) slot; likewise for the
//!   others; a function value of static type `() -> int` is exact at the same slot.
module Conf;

public class C
  public property [f: () -> string]: int => 1;
  public property [f: () -> int]: int => 2;
  public property [f: (int) -> int]: int => 3;
  public constructor()
  end constructor;
end C;

public class Program
  public static method Main(): int
    using var c := new C();
    Console.WriteLine(c[lambda() => 7]);
    Console.WriteLine(c[lambda() => "s"]);
    Console.WriteLine(c[lambda(x: int) => x + 1]);
    var g: () -> int := lambda() => 9;
    Console.WriteLine(c[g]);
    var h: () -> string := lambda() => "z";
    Console.WriteLine(c[h]);
    var k: (int) -> int := lambda(x: int) => x;
    Console.WriteLine(c[k]);
    return 0;
  end Main;
end Program;
