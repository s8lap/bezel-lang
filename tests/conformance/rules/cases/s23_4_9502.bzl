//! case: s23_4_9502
//! doc: spec
//! section: 23.4
//! title: a compound assignment and an assignment through a function-typed indexer hit the matching accessors
//! rule: Indexers may be overloaded by parameter type list.
//! cites: 23.4, 22.6
//! expect: run
//! output: 10025
//!   11025
//!   11100
//! notes: Pins #9187 (pick 3715595a8): an indexer overloaded only by a function-typed parameter binds the
//!   overload whose function type matches the argument's (sema and codegen agreed on the
//!   first-declared overload before the fix). `() => 7` selects the `() -> int` overload (field _b),
//!   `() => "x"` the string one (field _a): _b = 25 gives 10025; _a = 11 gives 11025; _b := 100 gives
//!   11100.
module Conf;

public class C
  private _a: int := 10;
  private _b: int := 20;
  public property [f: () -> string]: int
    get => _a;
    set => do _a := value; end;
  end;
  public property [f: () -> int]: int
    get => _b;
    set => do _b := value; end;
  end;
  public method Dump(): int => _a * 1000 + _b;
  public constructor()
  end constructor;
end C;

public class Program
  public static method Main(): int
    using var c := new C();
    c[lambda() => 7] += 5;
    Console.WriteLine(c.Dump());
    c[lambda() => "x"] += 1;
    Console.WriteLine(c.Dump());
    c[lambda() => 7] := 100;
    Console.WriteLine(c.Dump());
    return 0;
  end Main;
end Program;
