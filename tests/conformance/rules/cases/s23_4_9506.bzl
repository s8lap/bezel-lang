//! case: s23_4_9506
//! doc: spec
//! section: 23.4
//! title: an interface-declared indexer overloaded over five function types selects each by signature
//! rule: the candidate is applicable at the position when the lambda's inferred return type equals the
//!   candidate's return slot, implicitly converts to it
//! cites: 23.4, 22.6, 11.2
//! expect: run
//! output: 1
//!   15
//!   107
//!   1006
//!   10000
//! notes: Pins #9187 (pick 3715595a8): an indexer overloaded only by a function-typed parameter binds the
//!   overload whose function type matches the argument's (sema and codegen agreed on the
//!   first-declared overload before the fix). A unit-bodied block lambda is exact at the `() -> ()`
//!   slot (1); `() => 5` is exact at `() -> int` and converting at the procedure slot, so it selects
//!   the int one (10 + 5); the [3]int and [4]int block lambdas select their own slot (100 + 7, 1000 +
//!   6); the one-parameter lambda is inapplicable by parameter count everywhere but the last slot
//!   (q(5) runs, 10000).
module Conf;

public interface IFl
  readonly property [q: () -> ()]: int;
  readonly property [q: () -> int]: int;
  readonly property [q: () -> [3]int]: int;
  readonly property [q: () -> [4]int]: int;
  readonly property [q: (int) -> ()]: int;
end IFl;

public class FlI: IFl
  public property [q: () -> ()]: int get => do q(); return 1; end; end;
  public property [q: () -> int]: int get => do return 10 + q(); end; end;
  public property [q: () -> [3]int]: int get => do var a := q(); return 100 + a[0]; end; end;
  public property [q: () -> [4]int]: int get => do var a := q(); return 1000 + a[3]; end; end;
  public property [q: (int) -> ()]: int get => do q(5); return 10000; end; end;
  public constructor()
  end constructor;
end FlI;

public class Program
  public static method Main(): int
    using var c := new FlI();
    var fl := c;
    Console.WriteLine(fl[lambda() => do end]);
    Console.WriteLine(fl[lambda() => 5]);
    Console.WriteLine(fl[lambda() => do var a: [3]int := [7, 8, 9]; return a; end]);
    Console.WriteLine(fl[lambda() => do var a: [4]int := [7, 8, 9, 6]; return a; end]);
    Console.WriteLine(fl[lambda(x: int) => do end]);
    return 0;
  end Main;
end Program;
