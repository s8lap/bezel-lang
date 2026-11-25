//! case: s23_4_9505
//! doc: spec
//! section: 23.4
//! title: an indexer overloaded by function type selects by a method reference's signature
//! rule: The candidate is applicable at the position when formation succeeds under
//! cites: 23.4, 22.6, 27.6
//! expect: run
//! output: 2
//!   1
//! notes: Pins #9187 (pick 3715595a8): an indexer overloaded only by a function-typed parameter binds the
//!   overload whose function type matches the argument's (sema and codegen agreed on the
//!   first-declared overload before the fix). The reference is formed per candidate: `Seven: () ->
//!   int` forms at the int slot only (2); `Str: () -> string` at the string slot only (1).
module Conf;

public class C
  public property [f: () -> string]: int => 1;
  public property [f: () -> int]: int => 2;
  public constructor()
  end constructor;
end C;

public class Program
  public static method Seven(): int => 7;
  public static method Str(): string => "s";

  public static method Main(): int
    using var c := new C();
    Console.WriteLine(c[Seven]);
    Console.WriteLine(c[Str]);
    return 0;
  end Main;
end Program;
