//! case: s28_3_10_8606
//! doc: spec
//! section: 28.3.10
//! title: an overriding setter body disposes its owned local and the base setter does not run
//! rule: An owned local is disposed exactly once on every control-flow path.
//! cites: 28.3.10, 23.3
//! expect: run
//! output: D|
//! notes: Harvested from the #9073 review (probe s12_record). h is statically H but holds an H2, so h.P := 2 runs the overriding setter, whose local q is disposed at its end (D). The base setter (which would also print D) does not run, so exactly one D precedes the marker.
module Conf;
import System;
public class Res
  public n: int := 0;
  destructor
    Console.Write("D");
  end destructor;
end Res;
public class H
  private _v: int := 0;
  public constructor() end constructor;
  public virtual property P: int
    get => _v;
    set => do var r := new Res(); _v := value; end;
  end;
end H;
public class H2: H
  public constructor() end constructor;
  public override property P: int
    get => 1;
    set => do var q := new Res(); end;
  end;
end H2;
public static class Program
  public static method Main(args: Array<string>): int
    var h: H := new H2();
    h.P := 2; Console.Write("|");
    return 0;
  end;
end Program;
