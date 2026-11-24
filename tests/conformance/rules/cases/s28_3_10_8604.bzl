//! case: s28_3_10_8604
//! doc: spec
//! section: 28.3.10
//! title: an indexer setter body disposes its owned local, and the indexer parameter and value are readable
//! rule: An owned local is disposed exactly once on every control-flow path.
//! cites: 28.3.10, 23.3
//! expect: run
//! output: D9
//! notes: Harvested from the #9073 review (probe s7_indexer). h[2] := 7 runs the block-form setter: r is created and disposed at the setter's end (D), and _x := value + i + r.n = 7 + 2 + 0 = 9. The getter returns _x, so WriteLine prints 9 after the D: D9.
module Conf;
import System;
public class Res
  public n: int := 0;
  destructor
    Console.Write("D");
  end destructor;
end Res;
public class H
  public _x: int := 0;
  public constructor() end constructor;
  public property [i: int]: int
    get => _x;
    set => do var r := new Res(); _x := value + i + r.n; end;
  end;
end H;
public static class Program
  public static method Main(args: Array<string>): int
    var h := new H();
    h[2] := 7; Console.WriteLine(h[2]);
    return 0;
  end;
end Program;
