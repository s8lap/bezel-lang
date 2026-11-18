//! case: s28_3_10_7408
//! doc: spec
//! section: 28.3.10
//! title: an async lambda disposes an owned local on the path where it is not moved out
//! rule: An owned local is disposed exactly once on every control-flow path.
//! cites: 28.3.10
//! expect: run
//! output: total=200 n=0
//! notes: Conditional move-out inside an async lambda: exactly once on each path.
module Conf;
import System;
import System.Threading.Tasks;
public class Cell
  public static n: int := 0;
  public value: int := 0;
  public constructor(v: int) Cell.n := Cell.n + 1; this.value := v; end constructor;
  destructor Cell.n := Cell.n - 1; end destructor;
end Cell;
public class Program
  public static async method Take(owned c: Cell): Task<int>
    return c.value;
  end Take;
  public static async method Run(j: int): Task<int>
    var f := async lambda(k: int): Task<int> => do var u := new Cell(1); if k mod 2 = 0 then return await Program.Take(move u); end; return u.value; end;
    return await f(j);
  end Run;
  public static async method Main(): Task<int>
    var total := 0;
    for var j in 0..<200 do total := total + await Program.Run(j); end;
    Console.WriteLine("total=" + total.ToString() + " n=" + Cell.n.ToString());
    return 0;
  end Main;
end Program;
