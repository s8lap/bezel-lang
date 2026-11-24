//! case: s28_3_10_8601
//! doc: spec
//! section: 28.3.10
//! title: a plain setter body disposes its owned locals once on the normal path, loop path and catch path
//! rule: An owned local is disposed exactly once on every control-flow path.
//! cites: 28.3.10, 23.3
//! expect: run
//! output: DDDD|DDDcD|
//! notes: Harvested from the #9073 review (probe s2_loop_using_try). P := 1: the loop body local r is disposed at the end of each of its 2 iterations (DD), the try-block local t at the end of the try block (D), the using local u at the end of the setter (D): DDDD. P := 9: DD for the loop, t is disposed during the unwind of the throw (D) before the catch arm prints c, then u (D): DDDcD. Console.Write only, so no stdout/stderr interleaving is asserted.
module Conf;
import System;
public class Res
  public n: int := 0;
  destructor
    Console.Write("D");
  end destructor;
end Res;
public class H
  public constructor() end constructor;
  public property P: int
    set => do
      using var u := new Res();
      var k := 0;
      while k < 2 do var r := new Res(); k := k + 1; end while;
      try
        var t := new Res();
        if value > 5 then throw new Exception("x"); end if;
      catch
        Exception => Console.Write("c"),
      end try;
    end;
  end;
end H;
public static class Program
  public static method Main(args: Array<string>): int
    var h := new H();
    h.P := 1; Console.Write("|"); h.P := 9; Console.Write("|");
    return 0;
  end;
end Program;
