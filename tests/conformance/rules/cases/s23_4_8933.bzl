//! case: s23_4_8933
//! doc: spec
//! section: 23.4
//! title: an interface indexer with a function-typed index parameter honours a compound assignment
//! rule: Indexers may be overloaded by parameter type list.
//! cites: 23.4, 11.2, 28.14, 28.9
//! expect: run
//! output: 4400
//! notes: Pins #9137. The compound `r[g] += 1` reads (0 + 7 = 7), adds 1 (8), and stores `_last := q() + 8`
//!   = 15; the following read gives 15 + 7 = 22; 200 iterations: 4400.
module Conf;
public interface IFl
  property [q: () -> int]: int;
end IFl;

public class FlI : IFl
  private _last: int := 0;
  public property [q: () -> int]: int
    get => _last + q();
    set => do _last := q() + value; end;
  end;
  public constructor() end constructor;
end FlI;

public class Program
  public static method Seven(): int => 7;
  public static method Main(): int
    var t := 0;
    for var j in 0 ..< 200 do
      using var c := new FlI();
      var r: IFl := c;
      var g := lambda() => 7;
      r[g] += 1;
      t := t + r[g];
    end for;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;
