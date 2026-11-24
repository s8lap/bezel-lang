//! case: s28_3_10_8605
//! doc: spec
//! section: 28.3.10
//! title: a setter body reached through an interface disposes its owned local
//! rule: An owned local is disposed exactly once on every control-flow path.
//! cites: 28.3.10, 23.3
//! expect: run
//! output: D|
//! notes: Harvested from the #9073 review (probe s11_iface). The assignment i.P := 2 dispatches to H's block-form setter, whose local r is disposed at the setter's end (D); then the marker: D|.
module Conf;
import System;
public class Res
  public n: int := 0;
  destructor
    Console.Write("D");
  end destructor;
end Res;
public interface I
  property P: int;
end I;
public class H: I
  public constructor() end constructor;
  public property P: int
    get => 0;
    set => do var r := new Res(); end;
  end;
end H;
public static class Program
  public static method Main(args: Array<string>): int
    var h := new H();
    var i: I := h;
    i.P := 2; Console.Write("|");
    return 0;
  end;
end Program;
