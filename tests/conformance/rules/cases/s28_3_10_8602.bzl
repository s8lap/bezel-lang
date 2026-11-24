//! case: s28_3_10_8602
//! doc: spec
//! section: 28.3.10
//! title: a plain setter body disposes its owned local on the throw edge and on the normal edge
//! rule: Owned locals and owned parameters that are live when an exception propagates out of their enclosing block are disposed implicitly during stack unwinding
//! cites: 28.3.10, 28.3.11, 23.3
//! expect: run
//! stdout: Dc|D|
//! notes: Harvested from the #9073 review (probe s3_throw). P := 9 throws after r is created: r is disposed during the unwind (D) before the caller's catch arm prints c, then the | marker. P := 1 returns normally: r is disposed at the setter's end (D), then the second marker. Output Dc|D|.
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
      var r := new Res();
      if value > 5 then throw new Exception("x"); end if;
      r.n := value;
    end;
  end;
end H;
public static class Program
  public static method Main(args: Array<string>): int
    var h := new H();
    try
      h.P := 9;
    catch
      Exception => Console.Write("c"),
    end try;
    Console.Write("|");
    h.P := 1;
    Console.Write("|");
    return 0;
  end;
end Program;
