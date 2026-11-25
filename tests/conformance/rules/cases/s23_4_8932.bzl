//! case: s23_4_8932
//! doc: spec
//! section: 23.4
//! title: an interface indexer with a function-typed index parameter stores then reads with a fresh lambda
//! rule: Indexers may be overloaded by parameter type list.
//! cites: 23.4, 11.2, 28.14, 28.9
//! expect: run
//! output: 3400
//! notes: Pins #9137. The setter stores `_last := q() + value` = 7 + 3 = 10; the getter then reads `_last +
//!   q()` = 17; 200 iterations: 3400.
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
      r[lambda() => 7] := 3;
      t := t + r[lambda() => 7];
    end for;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;
