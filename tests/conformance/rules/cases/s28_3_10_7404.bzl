//! case: s28_3_10_7404
//! doc: spec
//! section: 28.3.10
//! title: an async lambda disposes its owned local although its body exits only by throwing
//! rule: An owned local is disposed exactly once on every control-flow path.
//! cites: 28.3.10
//! expect: run
//! output: total=200 n=0
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
  public static sink: int := 0;
  public static async method Run(j: int): Task<int>
    var f := async lambda(): Task<int> => do if j >= 0 then var u := new Cell(1); await Task.Delay(0); Program.sink := u.value; end; throw new InvalidOperationException("x"); end;
    var r := 0;
    try
      r := await f();
    catch
      InvalidOperationException => do r := 1; end
    end;
    return r;
  end Run;
  public static async method Main(): Task<int>
    var total := 0;
    for var j in 0..<200 do total := total + await Program.Run(j); end;
    Console.WriteLine("total=" + total.ToString() + " n=" + Cell.n.ToString());
    return 0;
  end Main;
end Program;
