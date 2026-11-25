//! case: s28_14_8906
//! doc: spec
//! section: 28.14
//! title: a caller-owned closure local used twice as an index argument in one statement survives both reads
//! rule: the callee receives the callback for the duration of the call only
//! cites: 28.14, 28.9, 23.4
//! expect: run
//! output: 25200
//! notes: Harvested from the #9115 review (the over-release hazard of a widened release predicate). `c` is a
//!   closure returned by a user method and `h` a second reference to it; `fl[c] + fl[c]` reads it twice
//!   as the index argument, then `h()` calls it again: 42 + 42 + 42 = 126 per iteration, 200
//!   iterations: 25200. Value-only control (green on both arms).
module Conf;
public class Gen
  private _k: int := 42;
  public method MkClosure(): () -> int
    var k := _k;
    return lambda() => k;
  end MkClosure;
  public constructor() end constructor;
end Gen;

public class Fl
  private _v: int := 0;
  public property [q: () -> int]: int
    get => _v + q();
    set => do _v := value + q(); end;
  end;
  public constructor() end constructor;
end Fl;

public class Program
  public static method Main(): int
    var t := 0;
    using var g := new Gen();
    for var j in 0 ..< 200 do
      var c := g.MkClosure();
      var h := c;
      using var fl := new Fl();
      t := t + fl[c] + fl[c];
      t := t + h();
    end for;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;
