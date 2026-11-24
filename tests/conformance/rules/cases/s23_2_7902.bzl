//! case: s23_2_7902
//! doc: spec
//! section: 23.2
//! title: a guarded cyclic expression-bodied function-typed property read yields the
//!   unguarded arm's lambda
//! rule: An expression-bodied property's (or indexer's) body is checked against the declared
//!   type
//! cites: 23.2
//! expect: run
//! stdout: RESULT 2
module M;
import System;
public class H
  private _d: int;
  public constructor() _d := 3; end;
  public property A: (int) -> int => (if _d > 5 then B else lambda(x: int) => x + 1);
  public property B: (int) -> int => (if _d > 6 then A else lambda(x: int) => x + 2);
end H;
public static class Program
  public static method Main(args: Array<string>): int
    using var h := new H();
    var tot := 0;
    var g := h.A;
    tot := tot + g(1);
    Console.WriteLine("RESULT " + tot.ToString());
    return 0;
  end;
end Program;
