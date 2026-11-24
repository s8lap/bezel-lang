//! case: s23_2_7901
//! doc: spec
//! section: 23.2
//! title: a dead-code read of a cyclic expression-bodied function-typed property compiles and
//!   runs
//! rule: An expression-bodied property's (or indexer's) body is checked against the declared
//!   type
//! cites: 23.2
//! expect: run
//! stdout: read-compiled
module M;
import System;
public class H
  public property A: (int) -> int => B;
  public property B: (int) -> int => A;
end H;
public static class Program
  public static method Main(args: Array<string>): int
    using var h := new H();
    if args.Count > 100 then
      var g := h.A;
      Console.WriteLine(g(1).ToString());
    end;
    Console.WriteLine("read-compiled");
    return 0;
  end;
end Program;
