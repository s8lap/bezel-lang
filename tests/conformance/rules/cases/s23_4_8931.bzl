//! case: s23_4_8931
//! doc: spec
//! section: 23.4
//! title: an interface-typed indexer with a function-typed index parameter reads through the interface slot
//! rule: Indexers may be overloaded by parameter type list.
//! cites: 23.4, 11.2, 28.14, 28.9
//! expect: run
//! output: 1400
//! notes: Pins #9137 (narrow; the interface slot for a function-typed index parameter). `r` is the interface
//!   `IFl`; the getter returns `_last + q()` = 7 per iteration (200 iterations: 1400). The 30-row
//!   matrix of the review (fresh/stored/method-group/returned/captured argument x get/set/compound) is
//!   represented by the rows 8931-8934.
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
      t := t + r[g];
    end for;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;
