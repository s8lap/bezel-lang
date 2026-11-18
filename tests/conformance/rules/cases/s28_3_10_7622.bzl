//! case: s28_3_10_7622
//! doc: spec
//! section: 28.3.10
//! title: a plain owned local of a getter body is disposed at the getter's scope end
//! rule: When an owned local — or an `owned` parameter — reaches a *normal-exit* scope end without having reached a discharge point, and is not in moved-from state, it is disposed implicitly at that scope end
//! cites: 28.3.10
//! expect: run
//! output: 600
//!   0
module Conf;
import System;
class Cell
  public static n: int := 0;
  public v: int;
  public constructor(x: int) this.v := x; Cell.n := Cell.n + 1; end constructor;
  destructor Cell.n := Cell.n - 1; end destructor;
end Cell;
class H
  public property V: int
    get => do
      var t := new Cell(3);
      return t.v;
    end;
  end V;
end H;
public class Program
  public static method Main(): int
    var h := new H();
    var acc := 0;
    for var i in 0 ..< 200 do
      acc := acc + h.V;
    end;
    Console.WriteLine(acc);
    Console.WriteLine(Cell.n);
    return 0;
  end Main;
end Program;
