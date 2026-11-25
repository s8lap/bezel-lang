//! case: s23_4_8939
//! doc: spec
//! section: 23.4
//! title: an interface indexer with an int and a function-typed parameter reads, stores and compounds
//! rule: Indexers may be overloaded by parameter type list.
//! cites: 23.4, 11.2, 28.14
//! expect: run
//! output: 7400
//! notes: Pins #9137 (K2). `[i: int, q: () -> int]`: `r[1, f] := 3` stores `_last = q() + 3 + 1 = 11`; `r[1,
//!   f] += 1` reads 11 + 7 + 1 = 19, adds 1 and stores `_last = 7 + 20 + 1 = 28`; `r[2, f]` reads 28 +
//!   7 + 2 = 37; 200 iterations: 7400.
module Conf;
public interface IFl
  property [i: int, q: () -> int]: int;
end IFl;

public class FlI : IFl
  private _last: int := 0;
  public property [i: int, q: () -> int]: int
    get => _last + q() + i;
    set => do _last := q() + value + i; end;
  end;
  public constructor() end constructor;
end FlI;

public class Program
  public static method Main(): int
    var t := 0;
    for var j in 0 ..< 200 do
      using var c := new FlI();
      var r: IFl := c;
      r[1, lambda() => 7] := 3;
      r[1, lambda() => 7] += 1;
      t := t + r[2, lambda() => 7];
    end for;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;
