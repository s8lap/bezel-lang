//! case: s23_4_9504
//! doc: spec
//! section: 23.4
//! title: a generic class's indexer overloaded by `() -> T` and `() -> string` selects by the substituted slot
//! rule: the candidate is applicable at the position when the lambda's inferred return type equals the
//!   candidate's return slot, implicitly converts to it
//! cites: 23.4, 22.6, 14.5
//! expect: run
//! output: 1
//!   2
//! notes: Pins #9187 (pick 3715595a8): an indexer overloaded only by a function-typed parameter binds the
//!   overload whose function type matches the argument's (sema and codegen agreed on the
//!   first-declared overload before the fix). At G<int> the first slot is `() -> int`: `() => 7` is
//!   exact there (1) and inapplicable at the string slot; `() => "s"` is inapplicable at the int slot
//!   and exact at the string one (2).
module Conf;

public class G<T>
  public property [f: () -> T]: int => 1;
  public property [f: () -> string]: int => 2;
  public constructor()
  end constructor;
end G;

public class Program
  public static method Main(): int
    using var g := new G<int>();
    Console.WriteLine(g[lambda() => 7]);
    Console.WriteLine(g[lambda() => "s"]);
    return 0;
  end Main;
end Program;
