//! case: s11_2_9103
//! doc: spec
//! section: 11.2
//! title: an interface indexer with a `() -> [3]int` parameter reads a captured owned object's
//!   value
//! rule: parameter types that are exactly equal under the interface's substitution
//! cites: 11.2, 23.4, 27.1
//! expect: run
//! output: 8
//! issue: 9137
//! notes: Pins #9137 (pick 6c7cbb002): an interface indexer whose parameter is a function type with
//!   a `()` return or an inline-array return was not matched against its implementation (11.2:
//!   parameter types exactly equal under the interface's substitution), so the call crashed.
//!   The lambdas are called by the indexer bodies, so the values are exact.
module Conf;

public class Box
  public v: int := 0;
  public constructor(x: int) v := x; end constructor;
end Box;

public interface IFl
  readonly property [q: () -> [3]int]: int;
end IFl;

public class FlI : IFl
  public constructor() end constructor;
  public property [q: () -> [3]int]: int
    get => do var a := q(); return a[0] + a[2]; end;
  end;
end FlI;

public class Program
  public static method Main(): int
    using var c := new FlI();
    using var b := new Box(5);
    var fl: IFl := c;
    Console.WriteLine(fl[lambda() => do var a: [3]int := [b.v, 2, 3]; return a; end]);
    return 0;
  end Main;
end Program;
